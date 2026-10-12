#!/bin/bash
# Start the window server headless, run the test client against it, and check the screenshot the server wrote.
set -u

SERVER="${1:?usage: run-headless.sh SERVER CLIENT PYTHON CHECKER}"
CLIENT="${2:?}"
PYTHON="${3:?}"
CHECKER="${4:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-test-$$"
SHOT="/tmp/agora-window-server-$$.ppm"
CLIENT_W=320
CLIENT_H=200
TOP_COLOUR=FF0000
BOTTOM_COLOUR=0000FF

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"

# THE SERVER IS TOLD TO COMPOSITE FEWER FRAMES THAN THE CLIENT ASKS FOR, so it is the one that stops:
# it writes the screenshot and exits, and the client then sees the connection close. The other way round leaves
# the server waiting for a frame that the client has already decided not to send.
# **THE SERVER IS STOPPED BY A SIGNAL RATHER THAN BY A FRAME COUNT, AND THAT IS THE DIFFERENCE BETWEEN A TEST AND
# A RACE.** With a budget of N frames the server leaves as soon as N renders have happened — and a client that
# draws flat colours drives renders as fast as the loop turns, so the budget is spent in a fraction of the time
# the second client needs to start. MEASURED: the second client never drew, the overlap held the first client's
# colour, and "the higher level is on top" passed because only ONE WINDOW EXISTED. A test that passes for the
# wrong reason is worse than one that fails.
"${SERVER}" --socket "${SOCK}" --frames 1000000 --screenshot "${SHOT}" >/dev/null 2>/tmp/agora-window-server.log &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "FAIL: the server never created its socket" >&2
	cat /tmp/agora-window-server.log >&2
	exit 1
fi

# **TWO WINDOWS, AND THE LEVELS AND THE CREATION ORDER DISAGREE.** The first client is created at a HIGH level
# and the second at a LOW one, OVERLAPPING it — so creation order alone would put the second on top, and only
# ordering by level puts the first there. That disagreement is the whole test: with the levels agreeing with the
# creation order it would pass whether the server ordered by level or not.
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width "${CLIENT_W}" --height "${CLIENT_H}" \
	--colour "${TOP_COLOUR}" --x 100 --y 80 --level 10 --frames 100000 >/dev/null 2>&1 &
FIRST=$!
sleep 0.5
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width "${CLIENT_W}" --height "${CLIENT_H}" \
	--colour "${BOTTOM_COLOUR}" --x 150 --y 110 --level 0 --frames 100000 >/dev/null 2>&1 &
SECOND=$!
# **AND THE RUN LASTS LONG ENOUGH FOR BOTH TO BE ON THE SCREEN** before the server is asked to stop. Then the
# signal stops the LOOP (see `on_signal` in main.c), the ordinary exit path runs, and the screenshot is written
# with both windows in it.
sleep 2
kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null
kill "${FIRST}" "${SECOND}" 2>/dev/null
client_rc=0

# THE SERVER EXITS BY ITSELF once it has composited its frames, which is what makes this a test rather than a
# program someone has to stop.
wait "${SPID}" 2>/dev/null
server_rc=$?
if [ "${client_rc}" -ne 0 ] || [ "${server_rc}" -ne 0 ]; then
	echo "FAIL: client exit ${client_rc}, server exit ${server_rc}" >&2
	cat /tmp/agora-window-server.log >&2
	exit 1
fi
if [ ! -f "${SHOT}" ]; then
	echo "FAIL: the server wrote no screenshot" >&2
	exit 1
fi

"${PYTHON}" "${CHECKER}" "${SHOT}" "${TOP_COLOUR}" "${BOTTOM_COLOUR}" "${CLIENT_W}x${CLIENT_H}"
