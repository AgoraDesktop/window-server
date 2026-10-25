#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# **`--ui-scale` IS A MULTIPLIER APPLIED AFTER EVERYTHING ELSE, AND 1.0 IS THE WHOLE OF THE DEFAULT.** This checks the
# wiring rather than the arithmetic (which `tests/scale.c` asks directly): that the factor reaches the framebuffer as a
# product, that the startup line shows the product as one, that the environment's spelling works and the flag wins, and
# that a factor nobody can use is refused by name. No display is needed: the headless backend keeps whatever it is told.
set -u

SERVER="${1:?usage: run-ui-scale.sh SERVER}"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-ui-$$"
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

# Run with a one-second deadline and no client, capturing stderr; the environment is cleared of any factor the person
# running the tests may have set, because a suite that inherits one would measure the wrong thing quietly.
run() {
	env -u AGORA_UI_SCALE "${SERVER}" --backend headless --input none --socket "${SOCK}" \
		--width 100 --height 50 --idle-timeout 1 "$@" 2>&1 >/dev/null
}

# **A BASE OF 2 AND A FACTOR OF 1.25 IS 2.5**, not 2.25 and not 1.25: the factor multiplies the scale the session
# already decided, so 100x50 points at 2.5 are a 250x125 framebuffer.
OUT="$(run --scale 2 --ui-scale 1.25)"
echo "${OUT}" | grep -q 'output 100x50, 250x125 device pixels'
check $? "--scale 2 --ui-scale 1.25 asks for a 250x125 framebuffer"
echo "${OUT}" | grep -q '(scale 2.5 = 2 x 1.25 UI, 144 dpi)'
check $? "the startup line shows the product and the display's own dpi"

# **AND A FACTOR BELOW 1 SHRINKS**, which is the half of "comfort" a fraction can also mean.
OUT="$(run --scale 2 --ui-scale 0.75)"
echo "${OUT}" | grep -q 'output 100x50, 150x75 device pixels'
check $? "--ui-scale 0.75 takes a 2x session down to 1.5x"
echo "${OUT}" | grep -q '(scale 1.5 = 2 x 0.75 UI, 144 dpi)'
check $? "and the startup line says so"

# **THE DEFAULT IS A NO-OP TO THE BYTE**, because every session that existed before this factor did is a session at
# 1.0: the line must be exactly the line it always was, with no multiplication clause at all.
OUT="$(run --scale 2)"
echo "${OUT}" | grep -q '(scale 2, 144 dpi)'
check $? "with no --ui-scale the startup line is unchanged"

# **AND `--dpi` IS THE SAME BASE.** 96 dpi is 1.33203125 device pixels per point after quantisation, so a quarter more
# is 1.6640625 = 426/256 — a fraction the wire can carry exactly, not a rounded 1.66.
OUT="$(run --dpi 96 --ui-scale 1.25)"
echo "${OUT}" | grep -q '(scale 1.66406 = 1.33203 x 1.25 UI, 96 dpi)'
check $? "--dpi and --ui-scale compose: the dpi is the display's, the factor is the person's"

# **A LOGIN SETS THIS ONCE, SO THE ENVIRONMENT IS A DOOR — AND THE FLAG WINS OVER IT.**
OUT="$(env AGORA_UI_SCALE=1.5 "${SERVER}" --backend headless --input none --socket "${SOCK}" \
	--width 100 --height 50 --idle-timeout 1 2>&1 >/dev/null)"
echo "${OUT}" | grep -q 'output 100x50, 150x75 device pixels'
check $? "AGORA_UI_SCALE 1.5 is honoured with no flag"
OUT="$(env AGORA_UI_SCALE=1.5 "${SERVER}" --backend headless --input none --socket "${SOCK}" \
	--width 100 --height 50 --ui-scale 2 --idle-timeout 1 2>&1 >/dev/null)"
echo "${OUT}" | grep -q 'output 100x50, 200x100 device pixels'
check $? "--ui-scale overrides AGORA_UI_SCALE rather than multiplying it"
# An empty variable is "nobody said", which is what a shell writes when a setting is unset but not removed.
OUT="$(env AGORA_UI_SCALE= "${SERVER}" --backend headless --input none --socket "${SOCK}" \
	--width 100 --height 50 --scale 2 --idle-timeout 1 2>&1 >/dev/null)"
echo "${OUT}" | grep -q '(scale 2, 144 dpi)'
check $? "an empty AGORA_UI_SCALE means nobody said"

# **AND A FACTOR NO SESSION CAN BE SIZED WITH IS REFUSED, NAMING WHERE IT CAME FROM.** Zero would make the screen out of
# existence, a negative is not a multiplier, a typo is not a number, and past the ceiling the framebuffer arithmetic
# stops being able to hold the answer.
"${SERVER}" --backend headless --input none --socket "${SOCK}" --ui-scale 0 >/dev/null 2>&1
[ $? -ne 0 ]
check $? "--ui-scale 0 is refused"
"${SERVER}" --backend headless --input none --socket "${SOCK}" --ui-scale -1.25 >/dev/null 2>&1
[ $? -ne 0 ]
check $? "a negative --ui-scale is refused"
"${SERVER}" --backend headless --input none --socket "${SOCK}" --ui-scale banana >/dev/null 2>&1
[ $? -ne 0 ]
check $? "a --ui-scale that is not a number is refused"
"${SERVER}" --backend headless --input none --socket "${SOCK}" --ui-scale 17 >/dev/null 2>&1
[ $? -ne 0 ]
check $? "a --ui-scale past the ceiling is refused"
OUT="$(env AGORA_UI_SCALE=nope "${SERVER}" --backend headless --input none --socket "${SOCK}" 2>&1 >/dev/null)"
[ $? -ne 0 ] && echo "${OUT}" | grep -q 'AGORA_UI_SCALE'
check $? "a broken AGORA_UI_SCALE is refused, and the message names the variable"

echo "ui-scale: ${FAILED} failed"
exit $((FAILED == 0 ? 0 : 1))
