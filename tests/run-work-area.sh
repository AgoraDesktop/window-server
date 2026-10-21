#!/bin/bash
# Start the window server headless and ask the work-area probe what the screen has left.
#
# There is no screenshot to compare here: a reservation moves no pixels. What is measured is the number the server
# says — `agora_output.work_area`, which is `NSScreen.visibleFrame` — before and after clients reserve strips.
set -u

SERVER="${1:?usage: run-work-area.sh SERVER PROBE}"
PROBE="${2:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
SOCK="aws-work-area-$$"
LOG="/tmp/agora-window-server-work-area-$$.log"

cleanup() {
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	[ -n "${SPID:-}" ] && wait "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"
}
trap cleanup EXIT

rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock"

# THE SIZE IS THE PROBE'S EXPECTATIONS: 1280x720 with the server's 24-pixel menu bar leaves 696, and every number in
# `work_area.c` is derived from those two. Pinned here so a change to either is a change in one visible place.
"${SERVER}" --socket "${SOCK}" --width 1280 --height 720 --frames 1000000 \
	--system-menu-icon hamburger >/dev/null 2>"${LOG}" &
SPID=$!
for _ in $(seq 1 50); do
	[ -S "${RUNTIME}/${SOCK}" ] && break
	sleep 0.1
done
if [ ! -S "${RUNTIME}/${SOCK}" ]; then
	echo "FAIL: the server never created its socket" >&2
	cat "${LOG}" >&2
	exit 1
fi

WAYLAND_DISPLAY="${SOCK}" "${PROBE}"
RC=$?
if [ "${RC}" -ne 0 ]; then
	echo "server log:" >&2
	cat "${LOG}" >&2
fi
rm -f "${LOG}"
exit "${RC}"
