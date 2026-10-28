#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# **`agora_window.set_cursor`, FROM THE REQUEST TO THE THEME'S ARTWORK — AND IT NEEDS NO DISPLAY.**
#
# The pointer's shape is drawn by whichever side owns the pointer, so no screenshot of a headless server can show it:
# the headless backend deliberately has no pointer at all. What this asserts is the half that IS visible without a
# display — the server resolved the window's name against the theme, and said which answer each name got:
#
#   a shape the theme has      -> that cursor's artwork and hotspot
#   `none`                     -> no pointer at all
#   a name the theme has not   -> the default, said in as many words
#   the empty string           -> the default
#   an over-long name          -> refused, not truncated into another name
#
# The phrase `AWS_CURSOR_DIAG` controls is the ONLY instrument for this: a nested session hands the pointer to the host,
# and a DRM output composites it into pixels nobody is looking at in a test. The nested half — that the HOST was actually
# given the theme's image — is `run-cursor-theme.sh`'s, which SKIPS without a DISPLAY.
set -u

SERVER="${1:?usage: run-cursor-name.sh SERVER PROBE THEME-DIRECTORY}"
PROBE="${2:?}"
THEME="${3:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-cursor-name-$$"
LOG="${RUNTIME}/${SOCK}-server.log"
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
rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${LOG}"

# **THE SERVER RUNS UNTIL ITS DEADLINE, and the probe connects to it while it does.** `--frames` cannot be used: this
# run asserts no pixels, and counting composites would end the server on the probe's first commit.
AWS_CURSOR_DIAG=1 "${SERVER}" --backend headless --input none --socket "${SOCK}" \
	--idle-timeout 10 --cursor-theme "${THEME}" >/dev/null 2>"${LOG}" &
SERVER_PID=$!

for _ in $(seq 1 100); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "  [FAIL] the server never created a socket"
	cat "${LOG}" >&2
	kill "${SERVER_PID}" 2>/dev/null
	exit 1
fi

OUT=$(WAYLAND_DISPLAY="${SOCK}" "${PROBE}" 2>&1)
STATUS=$?
echo "${OUT}"
check "${STATUS}" "the probe ran to the end against a live server"
printf '%s\n' "${OUT}" | grep -q '^ok: agora_shell advertises version'
check $? "and the shell advertises the version that carries set_cursor"

kill "${SERVER_PID}" 2>/dev/null
wait "${SERVER_PID}" 2>/dev/null

echo "--- what the server resolved ---"
grep 'DIAG cursor:' "${LOG}" || echo "  (the server reported nothing)"

# **AND THE ANSWERS THEMSELVES.** Each is anchored on the window's number and the name it was given, so a server that
# resolved a DIFFERENT name — the default for everything, say — cannot pass.
grep -Eq 'DIAG cursor: window [0-9]+ asks for text, which the theme has at [0-9]+x[0-9]+, hotspot \([0-9]+,[0-9]+\)' "${LOG}"
check $? "a name the theme has resolves to the theme's artwork and hotspot"
grep -q 'DIAG cursor: window .* asks for no pointer at all (`none`)' "${LOG}"
check $? "`none` is the blank pointer rather than an arrow"
grep -q 'DIAG cursor: window .* asks for agora-no-such-cursor, which the theme has not got — the default is shown' "${LOG}"
check $? "a name the theme has not got falls back to the default, and says so"
grep -Eq 'DIAG cursor: window [0-9]+ asks for left_ptr, which the theme has' "${LOG}"
check $? "and the empty string a client sends for no opinion is the default by name"
grep -q 'DIAG cursor: window .* asked for a shape name longer than' "${LOG}"
check $? "an over-long name is refused rather than truncated into another one"

# **AND NOTHING CRASHED OR COMPLAINED ABOUT THE PROTOCOL.** A request above the object's version is a protocol error,
# and libwayland's answer to one is to drop the client — which would have killed the probe above, but say it here too
# because a server that survived by ignoring the error would be the worse failure.
if grep -qi 'protocol error\|invalid method' "${LOG}"; then
	check 1 "the server raised no protocol error for a version-11 request"
else
	check 0 "the server raised no protocol error for a version-11 request"
fi

echo "cursor-name: ${FAILED} failed"
exit $((FAILED == 0 ? 0 : 1))
