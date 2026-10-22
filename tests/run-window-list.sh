# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

#!/bin/bash
# Start the window server headless and ask the window-list probe to be a Dock for a moment.
#
# Nothing here is photographed: the claim is what a SHELL COMPONENT is told about other applications' windows, and
# what it can do to them. The probe makes its own windows, so every step is a sequence in one process rather than a
# race between two.
set -u

SERVER="${1:?usage: run-window-list.sh SERVER PROBE}"
PROBE="${2:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-window-list-$$"
LOG="/tmp/agora-window-server-window-list-$$.log"

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	[ -n "${SPID:-}" ] && wait "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"

"${SERVER}" --socket "${SOCK}" --width 1280 --height 720 --frames 1000000 \
	--system-menu-icon hamburger >/dev/null 2>"${LOG}" &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "FAIL: the server never created its socket" >&2
	cat "${LOG}" >&2
	exit 1
fi

WAYLAND_DISPLAY="${SOCK}" "${PROBE}"
RC=$?
if [ "${RC}" -ne 0 ]; then
	echo "server log:" >&2
	cat "${LOG}" >&2
fi
rm -f "${LOG}"
exit "${RC}"
