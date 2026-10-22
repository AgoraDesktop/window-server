#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

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
STATUS_COLOUR=00FF00
STATUS_LENGTH=64
# kCGDesktopWindowLevel = kCGBaseWindowLevel (INT32_MIN) + 20 — the level BELOW every window, from
# libagora-drawing/CGWindowLevel.h. The server orders by the integer and does not care which; this is the one F4
# asks a window to sit at, so the test proves a DESKTOP-level window stays under a normal one.
DESKTOP_LEVEL=$(( -2147483648 + 20 ))

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
# **THE SYSTEM MENU IS PINNED TO THE HAMBURGER.** The server detects the host OS now, and this test is about
# windows and levels, not about which mark the bar happens to resolve on the machine running it. Forcing the
# fallback keeps the screenshot the same on Debian, Fedora and FreeBSD.
"${SERVER}" --socket "${SOCK}" --frames 1000000 --screenshot "${SHOT}" \
	--system-menu-icon hamburger >/dev/null 2>/tmp/agora-window-server.log &
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

# **TWO WINDOWS, AND THE LEVELS AND THE CREATION ORDER DISAGREE.** The first client is created at the NORMAL
# level and the second at the DESKTOP level, OVERLAPPING it — so creation order alone would put the second on top,
# and only ordering by level puts the first there. That disagreement is the whole test: with the levels agreeing
# with the creation order it would pass whether the server ordered by level or not. AND THE LOWER ONE IS A DESKTOP
# LEVEL, not another ordinary window level, because F4's claim is that a desktop-level window is below every window.
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width "${CLIENT_W}" --height "${CLIENT_H}" \
	--colour "${TOP_COLOUR}" --x 100 --y 80 --level 0 --frames 100000 >/dev/null 2>&1 &
FIRST=$!
sleep 0.5
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width "${CLIENT_W}" --height "${CLIENT_H}" \
	--colour "${BOTTOM_COLOUR}" --x 150 --y 110 --level "${DESKTOP_LEVEL}" --frames 100000 >/dev/null 2>&1 &
SECOND=$!
# **AND A THIRD CLIENT THAT IS NOT A WINDOW AT ALL.** It asks for a frame AND to be a status item, so what is
# measured is the SERVER's placement winning rather than a client that never asked to be anywhere. Nothing
# overlaps the bar, so this tests the packing without depending on the other two.
# **AND A LEVEL ABOVE THE BAR**, which is where a status item belongs (`NSStatusWindowLevel`, what the AppKit's
# `NSStatusItem` sets on its own window): the bar has a level now, so at the default the item is drawn first and
# the bar covers it.
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width "${CLIENT_W}" --height "${CLIENT_H}" \
	--colour "${STATUS_COLOUR}" --x 0 --y 0 --status-length "${STATUS_LENGTH}" --level 25 --frames 100000 >/dev/null 2>&1 &
THIRD=$!
# **AND THE RUN LASTS LONG ENOUGH FOR BOTH TO BE ON THE SCREEN** before the server is asked to stop. Then the
# signal stops the LOOP (see `on_signal` in main.c), the ordinary exit path runs, and the screenshot is written
# with both windows in it.
sleep 2
kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null
kill "${FIRST}" "${SECOND}" "${THIRD}" 2>/dev/null
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

"${PYTHON}" "${CHECKER}" "${SHOT}" "${TOP_COLOUR}" "${BOTTOM_COLOUR}" "${CLIENT_W}x${CLIENT_H}" \
	"${STATUS_COLOUR}" "${STATUS_LENGTH}"
