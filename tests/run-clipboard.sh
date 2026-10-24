#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# Two processes, one selection: an owner offers a type, a reader asks for it, and the bytes cross through a pipe.
#
# **NOTHING HERE TRUSTS THE COMPOSITOR TO CARRY ANYTHING.** The server creates the pipe and posts an end to each side; the
# payload goes owner -> kernel -> reader. So the assertion that matters is that the reader's stdout holds exactly what the
# owner wrote — the bytes really did cross, between two processes that never spoke to each other.
#
# And the three refusals are as much of the contract as the success is: "nothing there", "not that type" and "the owner
# went away" are different answers, because a paste that silently becomes the empty string is the failure people hate.
set -u

SERVER="${1:?usage: run-clipboard.sh SERVER CLIENT}"
CLIENT="${2:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-clip-$$"
FAIL=0
PAYLOAD="the bytes crossed"

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	[ -n "${OPID:-}" ] && kill "${OPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"

"${SERVER}" --backend headless --input none --socket "${SOCK}" --frames 1000000 --idle-timeout 30 \
	>/dev/null 2>/tmp/agora-clipboard.log &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "FAIL: the server never created its socket" >&2
	cat /tmp/agora-clipboard.log >&2
	exit 1
fi

# (1) NO OWNER AT ALL: the answer is a reason, not silence.
out=$(WAYLAND_DISPLAY="${SOCK}" timeout 5 "${CLIENT}" --request text/plain 2>&1)
if printf '%s' "${out}" | grep -q "refused (0)"; then
	echo "  ok    with no owner the answer is 'no_owner' (0), not silence"
else
	echo "FAIL: expected a no_owner refusal, got: ${out}" >&2
	FAIL=1
fi

# (2) AN OWNER, AND THE BYTES CROSS.
WAYLAND_DISPLAY="${SOCK}" timeout 8 "${CLIENT}" --offer text/plain --payload "${PAYLOAD}" >/dev/null 2>&1 &
OPID=$!
sleep 0.6
out=$(WAYLAND_DISPLAY="${SOCK}" timeout 5 "${CLIENT}" --request text/plain 2>&1)
if [ "${out}" = "selection text/plain: ${PAYLOAD}" ]; then
	echo "  ok    an owner's payload reaches a reader in ANOTHER PROCESS: '${PAYLOAD}'"
else
	echo "FAIL: the reader did not receive the payload — got: ${out}" >&2
	FAIL=1
fi

# (3) AN OWNER THAT DOES NOT OFFER THAT TYPE: a different reason, so a reader can tell them apart.
out=$(WAYLAND_DISPLAY="${SOCK}" timeout 5 "${CLIENT}" --request image/png 2>&1)
if printf '%s' "${out}" | grep -q "refused (1)"; then
	echo "  ok    a type nobody offers is 'wrong_type' (1), which a reader can tell from 'no_owner'"
else
	echo "FAIL: expected a wrong_type refusal, got: ${out}" >&2
	FAIL=1
fi

# (4) THE OWNER LEAVING TAKES THE SELECTION WITH IT.
kill "${OPID}" 2>/dev/null
wait "${OPID}" 2>/dev/null
OPID=""
sleep 0.3
out=$(WAYLAND_DISPLAY="${SOCK}" timeout 5 "${CLIENT}" --request text/plain 2>&1)
if printf '%s' "${out}" | grep -q "refused (0)"; then
	echo "  ok    an owner that goes away leaves NO selection behind — the promise the plan makes"
else
	echo "FAIL: a dead owner left a selection behind, got: ${out}" >&2
	FAIL=1
fi

exit "${FAIL}"
