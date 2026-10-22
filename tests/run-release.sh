# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

#!/bin/bash
# **A RELEASED BUFFER IS ONE THE COMPOSITOR HAS FINISHED WITH — AND ONLY ONE IT HAS REPLACED.**
#
# `wl_buffer.release` is a promise, and what makes it a promise is WHEN it is sent: a client is entitled to repaint
# those pixels the instant it arrives, so a release for the buffer that is still on the glass is the compositor
# reading memory a client may already be overwriting. That is not a theoretical fault: it is the blink on a focus
# change, measured at 62% of a window's pixels changing inside one captured frame (README.md §2c).
#
# The client keeps TWO buffers and alternates them, which is what every real client does and what makes the question
# answerable at all — with one buffer, "the released one" and "the displayed one" are the same object. Two commits
# after the first must therefore produce exactly one release each, and NEITHER may name the buffer that was on the
# glass when it arrived.
set -u

SERVER="${1:?usage: run-release.sh SERVER CLIENT}"
CLIENT="${2:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-release-$$"
FRAMES=4

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"

# THE SERVER IS STOPPED BY A SIGNAL, NOT BY A FRAME COUNT: the client is the thing being measured, and a server with
# a budget would leave before the last release was ever described. `--screenshot` is not asked for — this test is
# about the protocol, not about pixels, and the headless suite already photographs the same client.
"${SERVER}" --backend headless --input none --socket "${SOCK}" \
	--width 320 --height 200 --frames 1000000 >/dev/null 2>/tmp/agora-window-server-release.log &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "FAIL: the server never created its socket" >&2
	cat /tmp/agora-window-server-release.log >&2
	exit 1
fi

OUT=$(WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --double --frames "${FRAMES}" 2>&1)
rc=$?
echo "${OUT}"
if [ "${rc}" -ne 0 ]; then
	echo "FAIL: the client exited ${rc}" >&2
	cat /tmp/agora-window-server-release.log >&2
	exit 1
fi

releases=$(printf '%s\n' "${OUT}" | sed -n 's/.*releases=\([0-9][0-9]*\) while_current.*/\1/p')
while_current=$(printf '%s\n' "${OUT}" | sed -n 's/.*while_current=\([0-9][0-9]*\).*/\1/p')
if [ -z "${releases}" ] || [ -z "${while_current}" ]; then
	echo "FAIL: the client printed no release ledger" >&2
	exit 1
fi

# ONE RELEASE PER REPLACED BUFFER, AND THE FIRST COMMIT REPLACES NOTHING.
want=$(( FRAMES - 1 ))
if [ "${releases}" -ne "${want}" ]; then
	echo "FAIL: ${releases} release(s) for ${FRAMES} frames, want ${want} — a buffer is given back when the commit" >&2
	echo "      that replaces it arrives, and not before" >&2
	exit 1
fi
if [ "${while_current}" -ne 0 ]; then
	echo "FAIL: ${while_current} release(s) named the buffer that was ON THE GLASS — the compositor has taken pixels" >&2
	echo "      it is still drawing, which is the blink this test exists for" >&2
	exit 1
fi
echo "release: ${releases} release(s) for ${FRAMES} frames, none while current"
