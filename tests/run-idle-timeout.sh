#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# A server with nothing to composite must EXIT, not wait.
#
# `--frames N` counts composited frames, so a server with no client never reaches its own exit condition — it sits on a
# terminal (or in a test harness) until something kills it. MEASURED twice: once as a test that hung a shell, and once
# as a claimed terminal that left a session on a login prompt. `--idle-timeout` is the deadline that ends it, and it is
# about the FIRST frame only: a desktop with nothing to draw is a working desktop, so a second run with a client must
# keep running long after the deadline would have fired.
set -u

SERVER="${1:?usage: run-idle-timeout.sh SERVER CLIENT}"
CLIENT="${2:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-idle-$$"
FAIL=0

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"

# (1) NO CLIENT: the deadline fires, the server exits, and it says which deadline it was.
start=$(date +%s)
out=$("${SERVER}" --backend headless --input none --socket "${SOCK}" --frames 5 --idle-timeout 1 2>&1)
rc=$?
elapsed=$(( $(date +%s) - start ))
if [ "${elapsed}" -gt 4 ]; then
	echo "FAIL: the server waited ${elapsed}s on a 1s deadline" >&2
	FAIL=1
fi
if ! printf '%s' "${out}" | grep -q "nothing was composited within the deadline"; then
	echo "FAIL: the server said nothing about the deadline:" >&2
	printf '%s\n' "${out}" >&2
	FAIL=1
fi
echo "  ok    a server with no client EXITS on the deadline (${elapsed}s, rc=${rc})"

# (2) WITH A CLIENT: the deadline is disarmed by the first frame and the session runs on.
rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${RUNTIME}/${SOCK}.log"
# A FRAME BUDGET NOBODY SPENDS: a flat-colour client drives composites as fast as the loop turns, so a small `--frames`
# would be reached inside the window this test watches — and the server would exit for the RIGHT reason and be read as
# the wrong one. MEASURED: 40 frames went by in well under the three seconds below.
"${SERVER}" --backend headless --input none --socket "${SOCK}" --frames 1000000 \
	--idle-timeout 1 --screenshot "${RUNTIME}/${SOCK}.ppm" >/dev/null 2>"${RUNTIME}/${SOCK}.log" &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width 100 --height 60 --colour 00FF00 --x 40 --y 60 --frames 200 \
	>/dev/null 2>&1 &
CPID=$!
# LONGER THAN THE DEADLINE, ON PURPOSE: if it were still armed the server would be gone by now.
sleep 3
if ! kill -0 "${SPID}" 2>/dev/null; then
	echo "FAIL: the server exited even though a client was drawing" >&2
	cat "${RUNTIME}/${SOCK}.log" >&2
	FAIL=1
else
	echo "  ok    a client's first frame DISARMS the deadline and the session runs on"
fi
kill -TERM "${SPID}" "${CPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null
rm -f "${RUNTIME}/${SOCK}.log" "${RUNTIME}/${SOCK}.ppm"

exit "${FAIL}"
