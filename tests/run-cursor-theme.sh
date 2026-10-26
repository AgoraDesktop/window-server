#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# **THE POINTER'S THEME AS THE SERVER ACTUALLY RESOLVES IT, ON BOTH DISPLAY PATHS.**
#
# The headless half needs no display and checks the two doors a session has — `--cursor-theme` with the vendored theme,
# and a directory that is not a theme — through the startup line the server prints. Nothing can be photographed: the
# headless backend deliberately draws no pointer, because a compositor-owned pointer is a fact about an output nobody
# is looking at.
#
# The nested half is the half no screenshot can show, because a nested X11 output has the HOST draw its pointer: what
# this checks is that the server handed the host the theme's image as the window's cursor (`AWS_CURSOR_DIAG`), and that
# it leaves the host's own cursor alone when there is no theme. The alternative — warping the host's pointer into the
# window and asking XFixes what it is drawing — would move somebody's mouse to test a cursor, which is not a thing a
# test suite should do to a live session. It SKIPS (77) when the build has no X11 backend or there is no DISPLAY.
set -u

SERVER="${1:?usage: run-cursor-theme.sh SERVER THEME-DIRECTORY}"
THEME="${2:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-cursor-$$"
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

# **THE THEME REACHES THE COMPOSITED PATH AS AN IMAGE, AND THE STARTUP LINE SAYS WHICH THEME IT READ.** A server with
# no client exits on its own deadline, so nothing needs to hold it open.
OUT=$("${SERVER}" --backend headless --input none --socket "${SOCK}" --idle-timeout 1 \
	--cursor-theme "${THEME}" 2>&1 >/dev/null)
check $? "a session starts with the vendored theme"
echo "${OUT}" | grep -q "the pointer is left_ptr from the cursor theme at ${THEME}"
check $? "and its startup line names the theme and the shape"

OUT=$("${SERVER}" --backend headless --input none --socket "${SOCK}" --idle-timeout 1 \
	--cursor-size 48 --cursor-theme "${THEME}" 2>&1 >/dev/null)
echo "${OUT}" | grep -q "(48 points)"
check $? "and --cursor-size reaches it"

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
OUT=$("${SERVER}" --backend headless --input none --socket "${SOCK}" --idle-timeout 1 \
	--cursor-theme /nonexistent/agora/theme 2>&1 >/dev/null)
check $? "a session starts with no theme"
echo "${OUT}" | grep -q "no cursor theme at /nonexistent/agora/theme"
check $? "and says the theme was not there, rather than failing"

# **AND A THEME DIRECTORY THAT IS A THEME BUT HAS NO ARROW IS A DIFFERENT ANSWER.** `<dir>/cursors` exists, so it opens;
# `left_ptr` does not, so the pointer falls back — and the startup line says which of the two happened, because "the
# pointer is left_ptr from ..." would otherwise be a promise the theme does not keep.
EMPTY_THEME=$(mktemp -d "${RUNTIME}/aws-cursor-theme-XXXXXX")
mkdir -p "${EMPTY_THEME}/cursors"
rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
OUT=$("${SERVER}" --backend headless --input none --socket "${SOCK}" --idle-timeout 1 \
	--cursor-theme "${EMPTY_THEME}" 2>&1 >/dev/null)
check $? "a session starts with a theme that has no arrow"
echo "${OUT}" | grep -q "has no left_ptr for a 24-point pointer"
check $? "and names the shape it could not find"
rm -rf "${EMPTY_THEME}"

# **AND THE NESTED HALF NEEDS A BUILD WITH THE X11 BACKEND AND A DISPLAY.** Asked without a display, so the answer is
# about the BUILD: a build without the backend says so by name before it ever looks at DISPLAY.
if DISPLAY= "${SERVER}" --backend x11 --input none --socket "${SOCK}" --frames 1 --idle-timeout 1 \
	2>&1 >/dev/null | grep -q "no x11 backend"; then
	echo "SKIP: this build has no x11 backend" >&2
	echo "cursor-theme: ${FAILED} failed"
	exit 77
fi
if [ -z "${DISPLAY:-}" ]; then
	echo "SKIP: no DISPLAY for the nested backend" >&2
	echo "cursor-theme: ${FAILED} failed"
	exit 77
fi

# **A NESTED OUTPUT THAT ASKS FOR ONE FRAME AND DIES.** The window is mapped on the host's screen, so the host delivers
# an Expose and the server composites once; `--idle-timeout` is the belt to that brace. The pointer's size comes from
# the host display's own scale — 1.332 on a 96-dpi panel, 2 on a HiDPI one — so the assertion is that the host was
# handed a themed cursor of SOME variant, not one hard-coded size.
rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
OUT=$(AWS_CURSOR_DIAG=1 "${SERVER}" --backend x11 --renderer cpu --input none --socket "${SOCK}" \
	--frames 1 --idle-timeout 5 --cursor-theme "${THEME}" 2>&1 >/dev/null)
check $? "a nested server starts with the theme"
echo "${OUT}" | grep -q "the pointer is left_ptr from the cursor theme at ${THEME}"
check $? "and its startup line names the theme it read"
echo "${OUT}" | grep -Eq 'DIAG cursor: the host cursor is [0-9]+x[0-9]+ from the theme, hotspot \([0-9]+,[0-9]+\)'
check $? "and the HOST was handed the theme's image, not its own default cursor"
if echo "${OUT}" | grep -q "the host keeps its own cursor"; then
	check 1 "the host did not fall back to its own cursor"
else
	check 0 "the host did not fall back to its own cursor"
fi

# **AND WITHOUT A THEME THE HOST'S OWN CURSOR IS THE HONEST ANSWER.** A session that has no theme installed must not be
# given a cursor Agora drew and did not theme — the nested path hands over the desktop's pointer or nothing.
rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
OUT=$(AWS_CURSOR_DIAG=1 "${SERVER}" --backend x11 --renderer cpu --input none --socket "${SOCK}" \
	--frames 1 --idle-timeout 5 --cursor-theme /nonexistent/agora/theme 2>&1 >/dev/null)
check $? "a nested server starts with no theme"
echo "${OUT}" | grep -q "no cursor theme at /nonexistent/agora/theme"
check $? "and says the theme was not there"
echo "${OUT}" | grep -q "DIAG cursor: the host keeps its own cursor"
check $? "and leaves the host's own cursor in place"

echo "cursor-theme: ${FAILED} failed"
exit $((FAILED == 0 ? 0 : 1))
