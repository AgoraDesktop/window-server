#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# `--dpi` is the same override as `--scale`, stated as the display's dots per inch. This checks the CLI wiring —
# that the number reaches the framebuffer, that the startup line names both spellings, and that a contradictory or
# impossible request is refused. The arithmetic itself is asked directly by `tests/scale.c`.
set -u

SERVER="${1:?usage: run-dpi-override.sh SERVER}"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-dpi-$$"
FAILED=0

check() {
	if [ "$1" -eq 0 ]; then
		echo "  [ok]   $2"
	else
		echo "  [FAIL] $2"
		FAILED=$((FAILED + 1))
	fi
}

cleanup() {
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
}
trap cleanup EXIT
rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"

# **144 dpi IS 2 DEVICE PIXELS PER POINT**, so a 100x50-point request must be a 200x100-pixel framebuffer, and the
# startup line is where the server says both. It is printed before the event loop, and `--idle-timeout 1` makes a
# server with no client stop on its own, so no client is needed to hold the compositor open.
OUT="$("${SERVER}" --backend headless --input none --socket "${SOCK}" \
	--width 100 --height 50 --dpi 144 --idle-timeout 1 2>&1 >/dev/null)"
check $? "--dpi starts a server"
echo "${OUT}" | grep -q 'output 100x50, 200x100 device pixels'
check $? "--dpi 144 asks for a 2x framebuffer"
echo "${OUT}" | grep -q '(scale 2, 144 dpi)'
check $? "the startup line names both the scale and the dots per inch"

# **A FRACTIONAL OVERRIDE IS A REAL CONFIGURATION**, and it is the one the multi-head X11 report needed: two 1080p
# panels read as 3840 px against one panel's millimetres, so the derived scale is doubled and the fix is to state
# the panel's own dpi. 92 dpi must be 1.277... device pixels per point, not a rounded 1 or 2 — and the value on the
# startup line is the scale quantised to 1/256, which is what the protocol can carry and what a client is told.
OUT="$("${SERVER}" --backend headless --input none --socket "${SOCK}" \
	--width 100 --height 50 --dpi 92 --idle-timeout 1 2>&1 >/dev/null)"
echo "${OUT}" | grep -q '(scale 1.27734, 92 dpi)'
check $? "--dpi 92 keeps a fractional scale, quantised to 1/256"

# **ONE INSTRUCTION HAS ONE SPELLING**, so both together are refused rather than letting the later argument win.
"${SERVER}" --backend headless --dpi 96 --scale 2 >/dev/null 2>&1
[ $? -ne 0 ]
check $? "--dpi and --scale together are refused"

# **AND A NUMBER THAT IS NOT A DISPLAY IS REFUSED RATHER THAN READ AS "NOBODY SAID"**, which is what a typo would
# otherwise do: fall back to the derived value and make the wrong scale look like the server's fault.
"${SERVER}" --backend headless --dpi 0 >/dev/null 2>&1
[ $? -ne 0 ]
check $? "--dpi 0 is refused"
"${SERVER}" --backend headless --dpi -96 >/dev/null 2>&1
[ $? -ne 0 ]
check $? "a negative --dpi is refused"

echo "dpi-override: ${FAILED} failed"
exit $((FAILED == 0 ? 0 : 1))
