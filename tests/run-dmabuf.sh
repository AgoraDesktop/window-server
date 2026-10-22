# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later
# Portions of this file may derive from GNUstep; see THIRD-PARTY.md.

#!/bin/bash
# The same window twice — once in shared memory, once in GPU memory — photographed and compared.
#
# WHAT THIS PROVES. `zwp_linux_dmabuf_v1` is only worth having if the picture does not change: a client that hands
# over a dma-buf is handing over the same pixels by a different road, and the compositor's answer to "what is on the
# screen" must be identical. So the client runs twice with the same colour, position and level — once with `--dmabuf`
# and once without — and every pixel of the two screenshots is compared.
#
# IT NEEDS A DISPLAY AND A RENDER NODE, so it is a test that skips rather than one that fails on a machine that
# cannot run it: the X11 backend is how a compositor is watched without taking over a desktop, and `--dmabuf` needs
# gbm's render node. Exit 77 is the skip.
#
# Usage: run-dmabuf.sh SERVER CLIENT
set -u

SERVER="${1:?usage: run-dmabuf.sh SERVER CLIENT}"
CLIENT="${2:?usage: run-dmabuf.sh SERVER CLIENT}"
RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
PYTHON="${PYTHON3:-python3}"

export DISPLAY="${DISPLAY:-}"
if [ -z "${DISPLAY}" ]; then
	echo "SKIP: no DISPLAY for the nested backend" >&2
	exit 77
fi
if ! ls /dev/dri/renderD* >/dev/null 2>&1; then
	echo "SKIP: no DRM render node, so no dma-buf can be allocated" >&2
	exit 77
fi

PIDS=()
SHOTS=()
cleanup() {
	for p in "${PIDS[@]:-}"; do kill "${p}" 2>/dev/null; done
	for s in "${SHOTS[@]:-}"; do rm -f "${s}" "${s%.ppm}.log"; done
	rm -f "${RUNTIME}"/aws-dmabuf-* 2>/dev/null
}
trap cleanup EXIT

run_mode() {
	local mode="$1" extra="" shot log sock server client
	shot="/tmp/agora-dmabuf-${mode}-$$.ppm"
	log="/tmp/agora-dmabuf-${mode}-$$.log"
	sock="aws-dmabuf-${mode}-$$"
	SHOTS+=("${shot}")
	[ "${mode}" = "dmabuf" ] && extra="--dmabuf"

	"${SERVER}" --backend x11 --renderer gl --input host --socket "${sock}" --frames 1000000 \
		--screenshot "${shot}" --system-menu-icon hamburger >/dev/null 2>"${log}" &
	server=$!
	PIDS+=("${server}")
	for _ in $(seq 1 80); do [ -S "${RUNTIME}/${sock}" ] && break; sleep 0.05; done
	if [ ! -S "${RUNTIME}/${sock}" ]; then
		echo "FAIL: the server never listened (${mode})" >&2
		cat "${log}" >&2
		return 1
	fi
	WAYLAND_DISPLAY="${sock}" "${CLIENT}" --width 320 --height 200 --colour FF0000 \
		--x 100 --y 80 --level 0 --frames 100000 ${extra} >/dev/null 2>&1 &
	client=$!
	PIDS+=("${client}")
	sleep 1.2
	kill -TERM "${server}" 2>/dev/null
	wait "${server}" 2>/dev/null
	kill "${client}" 2>/dev/null
	rm -f "${RUNTIME}/${sock}" "${RUNTIME}/${sock}.lock"

	# THE RENDERER HAS TO HAVE BEEN THE GPU ONE, or this compared shared memory against itself.
	if ! grep -q "renderer: gl" "${log}"; then
		echo "FAIL: the gl renderer did not start (${mode})" >&2
		cat "${log}" >&2
		return 1
	fi
	if [ "${mode}" = "dmabuf" ] && ! grep -q "dmabuf: .*pair(s) advertised" "${log}"; then
		echo "FAIL: zwp_linux_dmabuf_v1 was not advertised (${mode})" >&2
		cat "${log}" >&2
		return 1
	fi
	[ -f "${shot}" ] || { echo "FAIL: no screenshot (${mode})" >&2; return 1; }
	echo "${shot}"
}

SHM_SHOT="$(run_mode shm)" || exit 1
DMABUF_SHOT="$(run_mode dmabuf)" || exit 1

"${PYTHON}" - "${SHM_SHOT}" "${DMABUF_SHOT}" <<'PY'
import sys

def load(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6", path
        width, height = map(int, f.readline().split())
        f.readline()
        return width, height, f.read()

w, h, a = load(sys.argv[1])
_, _, b = load(sys.argv[2])
if len(a) != len(b):
    print("FAIL: the two screenshots are different sizes")
    sys.exit(1)
differing = sum(1 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3])
if differing:
    print(f"FAIL: {differing} of {w * h} pixels differ between the shared-memory and dma-buf frames")
    sys.exit(1)
# And the window is actually there, so an all-background pair cannot pass by matching nothing.
i = (120 * w + 150) * 3
if tuple(a[i:i + 3]) != (255, 0, 0):
    print(f"FAIL: the window is not where it was asked to be: {tuple(a[i:i + 3])}")
    sys.exit(1)
print(f"ok: {w}x{h}, the dma-buf frame is pixel-identical to the shared-memory frame")
PY
