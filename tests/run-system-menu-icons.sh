#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# Force each System menu mark, let one real client commit a frame, and check the server's screenshot. **AND THEN RUN
# ONE MARK AT 2x**, because a mark that magnifies its 16×16 grid into a bigger box and a mark that is re-rasterized at
# the box's own size are the same picture at 1x and only different at a scale — the blocky one beside crisp text is
# the thing this checks against.
set -u

SERVER="${1:?usage: run-system-menu-icons.sh SERVER CLIENT PYTHON CHECKER}"
CLIENT="${2:?}"
PYTHON="${3:?}"
CHECKER="${4:?}"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
ICONS="alpinelinux archlinux debian fedora freebsd linuxmint netbsd openbsd opensuse redhat ubuntu hamburger"
FAILED=0

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	[ -n "${CPID:-}" ] && kill "${CPID}" 2>/dev/null
	[ -n "${SOCK:-}" ] && rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
	[ -n "${SHOT:-}" ] && rm -f "${SHOT}" "${LOG}"
}
trap cleanup EXIT

# ONE CASE: a server forced to draw ICON at SCALE, a client to give it a frame, and the checker on the screenshot.
one_icon() {
	local ICON="$1"
	local SCALE="$2"
	local EXTRA=("${@:3}")

	SOCK="aws-icon-$$-${ICON}-${SCALE}"
	SHOT="/tmp/agora-icon-$$-${ICON}-${SCALE}.ppm"
	LOG="/tmp/agora-icon-$$-${ICON}-${SCALE}.log"
	SPID=""
	CPID=""

	"${SERVER}" --socket "${SOCK}" --input none --frames 1000000 "${EXTRA[@]}" \
		--screenshot "${SHOT}" --system-menu-icon "${ICON}" >/dev/null 2>"${LOG}" &
	SPID=$!
	for _ in $(seq 1 50); do
		[ -S "${RUNTIME}/${SOCK}" ] && break
		sleep 0.1
	done
	if [ ! -S "${RUNTIME}/${SOCK}" ]; then
		echo "  [FAIL] ${ICON} at ${SCALE}x: the server never created its socket"
		cat "${LOG}" >&2
		FAILED=$((FAILED + 1))
		SPID=""
		return
	fi

	WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width 160 --height 120 --colour 0000FF \
		--x 100 --y 80 --level 0 --frames 100000 >/dev/null 2>&1 &
	CPID=$!
	sleep 0.7
	kill -TERM "${SPID}" 2>/dev/null
	wait "${SPID}" 2>/dev/null
	kill "${CPID}" 2>/dev/null
	wait "${CPID}" 2>/dev/null
	SPID=""
	CPID=""

	if [ ! -f "${SHOT}" ]; then
		echo "  [FAIL] ${ICON} at ${SCALE}x: the server wrote no screenshot"
		cat "${LOG}" >&2
		FAILED=$((FAILED + 1))
		return
	fi
	if ! "${PYTHON}" "${CHECKER}" "${SHOT}" "${ICON}" "${SCALE}"; then
		FAILED=$((FAILED + 1))
	fi
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}" "${LOG}"
}

for ICON in ${ICONS}; do
	one_icon "${ICON}" 1
done

# **AND THE SECOND SCALE, WHICH IS THE WHOLE REASON THE MARK IS RASTERIZED RATHER THAN MAGNIFIED.** 2x is the integer
# case a checker can name exactly: a 32-device-pixel box at (20,8), three bars of six solid device rows each.
one_icon hamburger 2 --scale 2

trap - EXIT
echo "system menu icons: ${FAILED} failed"
exit $((FAILED == 0 ? 0 : 1))
