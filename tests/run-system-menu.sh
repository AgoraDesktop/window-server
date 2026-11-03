#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# THE SYSTEM MENU'S CHIP, PHOTOGRAPHED. A probe claims the square and reports its menu up; the server draws the highlight
# the square wears while the menu is down; the checker measures it against the constants in `server.h`.
#
# **TWO SCALES, BECAUSE THE RECTANGLE IS ROUNDED ONCE PER EDGE.** At 1 the numbers are the constants themselves; at 1.5
# every edge is `round(logical * 1.5)` and a renderer that rounded differently — or that built the chip from the SQUARE
# instead of the MARK — gives a rectangle whose air is not equal. The scale is a device fact the test varies on purpose.
set -u

SERVER="${1:?usage: run-system-menu.sh SERVER PROBE PYTHON CHECKER}"
PROBE="${2:?}"
PYTHON="${3:?}"
CHECKER="${4:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
failures=0

for SCALE in 1 1.5; do
	SOCK="aws-system-menu-$$-${SCALE/./_}"
	SHOT="/tmp/agora-system-menu-$$-${SCALE/./_}.ppm"

	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"

	"${SERVER}" --backend headless --input none --scale "${SCALE}" --socket "${SOCK}" \
		--width 640 --height 400 --frames 1000000 --screenshot "${SHOT}" \
		--system-menu-icon debian >/dev/null 2>/tmp/agora-system-menu.log &
	SPID=$!
	for _ in $(seq 1 50); do
		[ -S "${RUNTIME}/${SOCK}" ] && break
		sleep 0.1
	done
	if [ ! -S "${RUNTIME}/${SOCK}" ]; then
		echo "FAIL: the server never created its socket" >&2
		cat /tmp/agora-system-menu.log >&2
		kill "${SPID}" 2>/dev/null
		exit 1
	fi

	WAYLAND_DISPLAY="${SOCK}" "${PROBE}" --hold 3 >/tmp/agora-system-menu-probe.log 2>&1 &
	PPID_PROBE=$!
	sleep 1.5
	# A SIGNAL, NOT A FRAME BUDGET: the screenshot is written when the server stops, and the chip has to be on the glass
	# before that happens.
	kill -TERM "${SPID}" 2>/dev/null
	wait "${SPID}" 2>/dev/null
	kill "${PPID_PROBE}" 2>/dev/null
	wait "${PPID_PROBE}" 2>/dev/null

	if [ ! -f "${SHOT}" ]; then
		echo "FAIL: the server wrote no screenshot at scale ${SCALE}" >&2
		cat /tmp/agora-system-menu.log >&2
		failures=$((failures + 1))
		continue
	fi
	echo "scale ${SCALE}:"
	"${PYTHON}" "${CHECKER}" "${SHOT}" "${SCALE}" || failures=$((failures + 1))
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"
done

echo "${failures} scale(s) failed"
[ "${failures}" = 0 ]
