#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# WHO GETS A SQUARE CORNER: a window at the top of the work area, or a menu hanging from the bar.
#
# Both start at y = AWS_MENU_BAR_HEIGHT, because the work area begins at the bar's bottom edge — the same y a
# dropdown has. The compositor used to tell them apart by that number alone, so every window placed at the top of
# the screen got the menu's square top corners and looked like a window that had never been rounded.
set -u

SERVER="${1:?usage: run-corners.sh SERVER CLIENT PYTHON CHECKER}"
CLIENT="${2:?}"
PYTHON="${3:?}"
CHECKER="${4:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-corners-$$"
SHOT="/tmp/agora-corners-$$.ppm"
WINDOW_COLOUR=FF0000
MENU_COLOUR=008000
AT_Y=24			# AWS_MENU_BAR_HEIGHT: the top of the work area, and a menu's own top edge
MENU_LEVEL=101		# NSPopUpMenuWindowLevel — what makes a surface a piece of the bar

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"

# A SCALE OF 1, so a corner is at the number the client asked for and the checker can name it directly.
"${SERVER}" --backend headless --input none --scale 1 --socket "${SOCK}" \
	--width 640 --height 400 --frames 1000000 --screenshot "${SHOT}" \
	--system-menu-icon hamburger >/dev/null 2>/tmp/agora-corners.log &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "FAIL: the server never created its socket" >&2
	cat /tmp/agora-corners.log >&2
	exit 1
fi

WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width 200 --height 150 --x 100 --y "${AT_Y}" \
	--colour "${WINDOW_COLOUR}" --frames 100000 >/dev/null 2>&1 &
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width 200 --height 150 --x 360 --y "${AT_Y}" \
	--level "${MENU_LEVEL}" --colour "${MENU_COLOUR}" --frames 100000 >/dev/null 2>&1 &

# A SIGNAL, NOT A FRAME BUDGET: the two clients have to have drawn before the picture means anything.
sleep 2
kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null

"${PYTHON}" "${CHECKER}" "${SHOT}" "${WINDOW_COLOUR}" "${MENU_COLOUR}" "${AT_Y}"
