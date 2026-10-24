#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# One client that renders at 2x, against a live headless server, photographed.
#
# The claim is that `wl_surface.set_buffer_scale` places a scaled buffer in the LOGICAL frame the client asked for and
# downsamples the whole of it into that frame. Three ways to get it wrong are measured: ignoring the scale (the window
# overruns its frame by 2), drawing only the buffer's first quarter (one colour instead of four), and sampling the
# first pixel of every block (the wrong four).
set -u

SERVER="${1:?usage: run-buffer-scale.sh SERVER CLIENT PYTHON CHECKER}"
CLIENT="${2:?}"
PYTHON="${3:?}"
CHECKER="${4:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-scale-$$"
SHOT="/tmp/agora-buffer-scale-$$.ppm"
FRAME_W=200
FRAME_H=120
FRAME_X=120
FRAME_Y=120

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"

# THE SERVER IS STOPPED BY A SIGNAL, NOT BY A FRAME COUNT: the client draws flat colours and drives renders as fast as
# the loop turns, so a small budget would be spent before the client had even mapped.
"${SERVER}" --backend headless --input none --socket "${SOCK}" --width 800 --height 600 \
	--frames 1000000 --screenshot "${SHOT}" --system-menu-icon hamburger >/dev/null 2>/tmp/agora-scale.log &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "FAIL: the server never created its socket" >&2
	cat /tmp/agora-scale.log >&2
	exit 1
fi

# A 2x BUFFER IN A 1x FRAME, with four colours in it.
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width "${FRAME_W}" --height "${FRAME_H}" \
	--x "${FRAME_X}" --y "${FRAME_Y}" --buffer-scale 2 --quadrants --frames 100000 \
	>/dev/null 2>&1 &
CPID=$!
sleep 2
kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null
kill "${CPID}" 2>/dev/null

if [ ! -f "${SHOT}" ]; then
	echo "FAIL: the server wrote no screenshot" >&2
	cat /tmp/agora-scale.log >&2
	exit 1
fi

"${PYTHON}" "${CHECKER}" "${SHOT}" "${FRAME_X}" "${FRAME_Y}" "${FRAME_W}" "${FRAME_H}"
