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
CLIENT_COLOUR=00C050FF

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"

# THE SERVER IS TOLD TO COMPOSITE FEWER FRAMES THAN THE CLIENT ASKS FOR, so it is the one that stops:
# it writes the screenshot and exits, and the client then sees the connection close. The other way round leaves
# the server waiting for a frame that the client has already decided not to send.
"${SERVER}" --socket "${SOCK}" --frames 4 --screenshot "${SHOT}" >/dev/null 2>/tmp/agora-window-server.log &
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

WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width "${CLIENT_W}" --height "${CLIENT_H}" \
	--colour "${CLIENT_COLOUR}" --frames 5
client_rc=$?

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

"${PYTHON}" "${CHECKER}" "${SHOT}" "${CLIENT_COLOUR}" "${CLIENT_W}x${CLIENT_H}"
