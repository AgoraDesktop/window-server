#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

# Force each System menu mark, let one real client commit a frame, and check the server's screenshot.
set -u

SERVER="${1:?usage: run-system-menu-icons.sh SERVER CLIENT PYTHON CHECKER}"
CLIENT="${2:?}"
PYTHON="${3:?}"
CHECKER="${4:?}"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
ICONS="alpinelinux archlinux debian fedora freebsd linuxmint netbsd openbsd opensuse redhat ubuntu hamburger"
FAILED=0

for ICON in ${ICONS}; do
	SOCK="aws-icon-$$-${ICON}"
	SHOT="/tmp/agora-icon-$$-${ICON}.ppm"
	LOG="/tmp/agora-icon-$$-${ICON}.log"
	SPID=""
	CPID=""

	cleanup() {
		[ -n "${SPID}" ] && kill "${SPID}" 2>/dev/null
		[ -n "${CPID}" ] && kill "${CPID}" 2>/dev/null
		rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}" "${LOG}"
	}
	trap cleanup EXIT

	"${SERVER}" --socket "${SOCK}" --input none --frames 1000000 \
		--screenshot "${SHOT}" --system-menu-icon "${ICON}" >/dev/null 2>"${LOG}" &
	SPID=$!
	for _ in $(seq 1 50); do
		[ -S "${RUNTIME}/${SOCK}" ] && break
		sleep 0.1
	done
	if [ ! -S "${RUNTIME}/${SOCK}" ]; then
		echo "  [FAIL] ${ICON}: the server never created its socket"
		cat "${LOG}" >&2
		FAILED=$((FAILED + 1))
		SPID=""
		continue
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
		echo "  [FAIL] ${ICON}: the server wrote no screenshot"
		cat "${LOG}" >&2
		FAILED=$((FAILED + 1))
		continue
	fi
	if ! "${PYTHON}" "${CHECKER}" "${SHOT}" "${ICON}"; then
		FAILED=$((FAILED + 1))
	fi
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}" "${LOG}"
done

trap - EXIT
echo "system menu icons: ${FAILED} failed"
exit $((FAILED == 0 ? 0 : 1))
