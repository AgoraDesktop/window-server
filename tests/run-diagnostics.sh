#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

# The diagnostic doors must answer without a display, a backend or an X server.
set -u

SERVER="${1:?usage: run-diagnostics.sh SERVER}"
FAILED=0

check() {
	if [ "$1" -eq 0 ]; then
		echo "  [ok]   $2"
	else
		echo "  [FAIL] $2"
		FAILED=$((FAILED + 1))
	fi
}

OUT="$("${SERVER}" --dump-os-identity 2>/dev/null)"
STATUS=$?
check "${STATUS}" "--dump-os-identity exits 0 without a display"
echo "${OUT}" | grep -q '^sysname: ' && check 0 "it names the sysname" || check 1 "it names the sysname"
echo "${OUT}" | grep -q '^source: ' && check 0 "it names the source" || check 1 "it names the source"
echo "${OUT}" | grep -q '^ID: ' && check 0 "it names the ID" || check 1 "it names the ID"
echo "${OUT}" | grep -q '^ID_LIKE: ' && check 0 "it names ID_LIKE" || check 1 "it names ID_LIKE"
echo "${OUT}" | grep -q '^family: ' && check 0 "it names the family" || check 1 "it names the family"
echo "${OUT}" | grep -q '^system menu icon: ' && check 0 "it names the chosen mark" || check 1 "it names the chosen mark"

LIST="$("${SERVER}" --list-system-menu-icons 2>/dev/null)"
STATUS=$?
check "${STATUS}" "--list-system-menu-icons exits 0 without a display"
for ICON in hamburger debian ubuntu linuxmint fedora redhat archlinux opensuse alpinelinux freebsd openbsd netbsd; do
	echo "${LIST}" | grep -qx "${ICON}"
	check $? "the list contains ${ICON}"
done

OUT="$("${SERVER}" --dump-os-identity --system-menu-icon mint 2>/dev/null)"
echo "${OUT}" | grep -qx 'system menu icon: linuxmint'
check $? "an alias override reaches the chosen mark"

OUT="$("${SERVER}" --dump-os-identity --system-menu-icon dragonfly 2>/dev/null)"
echo "${OUT}" | grep -qx 'system menu icon: freebsd'
check $? "DragonFly's alias uses the FreeBSD mark for now"

"${SERVER}" --dump-os-identity --system-menu-icon no-such-mark >/dev/null 2>&1
[ $? -ne 0 ]
check $? "an unknown override fails"

echo "diagnostics: ${FAILED} failed"
exit $((FAILED == 0 ? 0 : 1))
