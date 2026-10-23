#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# Bind the seat at versions 1, 2, 3, 4, 5 and 8 against a live headless server and check WHICH keyboard events each
# one may receive. `wl_keyboard.repeat_info` is a version-4 event, so 1–3 must not see it and 4, 5 and 8 must.
#
# **AND VERSION 8 IS THE POINT OF THE LAST ONE.** A child object inherits its parent's bound version, so a seat
# advertised at 5 capped every pointer at 5 — which made `wl_pointer.axis_value120`, a version-8 event, unreachable
# no matter how carefully the server implemented it. The probe reports the version it was ACTUALLY bound at, so a
# seat that silently went back to 5 fails here: `bound=5` where 8 was asked for is a wheel's steps becoming
# unspellable.
#
# THE BOUNDARY IS THE TEST, and it is why all five versions are bound rather than only the server's maximum: a
# guard written as "if the seat is version 5, send it" passes a v5-only test and still breaks every older client.
set -u

SERVER="${1:?usage: run-seat-versions.sh SERVER PROBE}"
PROBE="${2:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-seat-version-$$"

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"

# A FRAME BUDGET NOBODY SPENDS: the server must stay up for all five probes, and it exits on the signal below.
"${SERVER}" --backend headless --input none --socket "${SOCK}" \
	--width 320 --height 200 --frames 1000000 >/dev/null 2>/tmp/agora-seat-version.log &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "FAIL: the server never created its socket" >&2
	cat /tmp/agora-seat-version.log >&2
	exit 1
fi

fail=0
for version in 1 2 3 4 5 8; do
	line=$(WAYLAND_DISPLAY="${SOCK}" "${PROBE}" "${version}" 2>&1)
	rc=$?
	if [ "${rc}" -ne 0 ]; then
		echo "FAIL: version ${version} bound at all (exit ${rc})" >&2
		echo "${line}" >&2
		fail=1
		continue
	fi
	got=$(printf '%s' "${line}" | sed -n 's/.*repeat_info=\([01]\).*/\1/p')
	keymap=$(printf '%s' "${line}" | sed -n 's/.*keymap=\([0-9]*\).*/\1/p')
	echo "  version ${version}: ${line}"
	bound=$(printf '%s' "${line}" | sed -n 's/.*bound=\([0-9]*\).*/\1/p')
	if [ "${bound}" != "${version}" ]; then
		echo "FAIL: asked for version ${version} and was BOUND at ${bound} — a child object inherits this number, so a" >&2
		echo "      seat capped low caps every pointer and keyboard under it" >&2
		fail=1
	fi
	# EVERY VERSION GETS THE KEYMAP: `keymap` is a version-1 event and the keyboard is useless without it.
	if [ "${keymap}" -lt 1 ]; then
		echo "FAIL: version ${version} received no keymap" >&2
		fail=1
	fi
	if [ "${version}" -lt 4 ]; then
		if [ "${got}" -ne 0 ]; then
			echo "FAIL: version ${version} received repeat_info, which does not exist before version 4" >&2
			fail=1
		fi
	else
		if [ "${got}" -ne 1 ]; then
			echo "FAIL: version ${version} received no repeat_info" >&2
			fail=1
		fi
	fi
done

kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null

if [ "${fail}" -ne 0 ]; then
	cat /tmp/agora-seat-version.log >&2
	exit 1
fi
exit 0
