#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# Start the window server headless and destroy a focused, mapped, topmost window under it.
#
# The claim is about an ORDERING inside the surface destructor, and what a client can see of it is one keyboard event:
# the window underneath must be told it has the keyboard. That is the assertion; the use-after-free the ordering
# caused is what it protects against, and an ASan build of the server makes it explicit.
set -u

SERVER="${1:?usage: run-surface-lifetime.sh SERVER PROBE}"
PROBE="${2:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-surface-lifetime-$$"
LOG="/tmp/agora-window-server-surface-lifetime-$$.log"

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	[ -n "${SPID:-}" ] && wait "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"

"${SERVER}" --socket "${SOCK}" --width 800 --height 600 --frames 1000000 \
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
# **A SERVER THAT DIED FROM THE DESTRUCTOR IS A FAILURE EVEN IF THE PROBE EXITED ZERO**, because the probe can
# finish its assertions and then lose the connection on the way out.
if ! kill -0 "${SPID}" 2>/dev/null; then
	wait "${SPID}" 2>/dev/null
	SRC=$?
	if [ "${SRC}" -ne 0 ] && [ "${SRC}" -ne 143 ]; then
		echo "FAIL: the server exited ${SRC} during the run" >&2
		cat "${LOG}" >&2
		RC=1
	fi
fi
rm -f "${LOG}"
exit "${RC}"
