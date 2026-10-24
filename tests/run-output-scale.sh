#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# A 2x output, photographed, with one ordinary client and one that renders at 2x.
#
# The claim is that `--scale 2` makes a HiDPI screen usable WITHOUT touching the clients: an ordinary window is drawn
# twice as large (the right size, soft pixels), and a client that renders at 2x itself is a 1:1 copy (sharp). The menu
# bar is furniture, not a client, so it must scale with the output rather than shrink into the corner.
set -u

SERVER="${1:?usage: run-output-scale.sh SERVER CLIENT PYTHON CHECKER}"
CLIENT="${2:?}"
PYTHON="${3:?}"
CHECKER="${4:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-outscale-$$"
SHOT="/tmp/agora-output-scale-$$.ppm"
LOGICAL_W=400
LOGICAL_H=300
BAR_H=24

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"

# THE LOGICAL SIZE IS WHAT IS ASKED FOR; the framebuffer is that times the scale.
"${SERVER}" --backend headless --input none --socket "${SOCK}" \
	--width "${LOGICAL_W}" --height "${LOGICAL_H}" --scale 2 \
	--frames 1000000 --screenshot "${SHOT}" --system-menu-icon hamburger >/dev/null 2>/tmp/agora-outscale.log &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "FAIL: the server never created its socket" >&2
	cat /tmp/agora-outscale.log >&2
	exit 1
fi

# AN ORDINARY CLIENT: a 1x buffer, so the compositor owes it an upscale.
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width 80 --height 40 --x 40 --y 40 \
	--colour FF0000 --frames 100000 >/dev/null 2>&1 &
A=$!
# AND ONE THAT RENDERS AT 2x ITSELF, in the same logical rect shape, further along.
WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width 80 --height 40 --x 200 --y 40 \
	--buffer-scale 2 --quadrants --frames 100000 >/dev/null 2>&1 &
B=$!
sleep 2
kill -TERM "${SPID}" 2>/dev/null
wait "${SPID}" 2>/dev/null
kill "${A}" "${B}" 2>/dev/null

if [ ! -f "${SHOT}" ]; then
	echo "FAIL: the server wrote no screenshot" >&2
	cat /tmp/agora-outscale.log >&2
	exit 1
fi

"${PYTHON}" "${CHECKER}" "${SHOT}" "${LOGICAL_W}" "${LOGICAL_H}" "${BAR_H}"
