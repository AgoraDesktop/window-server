#!/bin/bash
# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# **A CLIENT NAMES A STRIP, AND THE SCREEN PROVES THE COMPOSITOR PAINTED IT AND NOTHING ELSE.**
#
# Three scripted sessions, each one server and one client, each stopped by a SIGNAL rather than by a frame count — the
# same rule `run-headless.sh` follows, and for the same reason: a frame budget races the client and a test that passes
# because the client never drew is worse than a failing one.
#
#   strip   two commits: the whole window, then a strip — and a POISON in every pixel the strip does not cover. Green on
#           the strip, the base colour everywhere else, and no magenta anywhere is the whole claim.
#   whole   a third commit that attaches a buffer and damages NOTHING, which Wayland says means THE WHOLE BUFFER. The
#           third picture must replace the poison too.
#   move    `set_frame` moves the window and damages nothing: a full-redraw trigger, so both the new place and the ground
#           the window left have to be repainted. The rectangles could never have named the second one.
#
# AND THE DIAGNOSTIC IS CHECKED AS WELL AS THE PIXELS. `WINDOW_SERVER_DAMAGE_DIAG=1` prints the box each frame painted
# and, for a full frame, the trigger that asked for it — so the strip run's log must contain the strip's own box and
# exactly one FULL, from the map. A screenshot cannot show a damage box that was correct but unreasonably large; the line
# of text can.
set -u

SERVER="${1:?usage: run-damage.sh SERVER CLIENT PYTHON CHECKER}"
CLIENT="${2:?}"
PYTHON="${3:?}"
CHECKER="${4:?}"

RUNTIME="${XDG_RUNTIME_DIR:-/tmp}"
BASE_COLOUR=FF0000
STRIP_COLOUR=00FF00
THIRD_COLOUR=0000FF
W=320
H=200
OLD_X=100
OLD_Y=80
NEW_X=600
NEW_Y=300
STRIP_Y0=40
STRIP_Y1=60
# The strip's DEVICE rows: the window's own y plus the buffer rows, because this client's buffer is placed 1:1.
STRIP_BOX_AT_X=${OLD_X}
STRIP_BOX_AT_Y=$(( OLD_Y + STRIP_Y0 ))

FAILED=0
SKIPPED_GL=0
# WHAT BACKEND AND RENDERER THE NEXT CASE RUNS ON. The three headless cases are the CPU renderer and a framebuffer in
# this process; the fourth is the same scene through EGL on a nested X window — see its own note about swap preservation.
SERVER_EXTRA=""

cleanup_case() {
	[ -n "${CPID:-}" ] && kill "${CPID}" 2>/dev/null
	[ -n "${SPID:-}" ] && kill "${SPID}" 2>/dev/null
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}"
	CPID=""
	SPID=""
}

# ONE SESSION: start the server, start the client, let it draw, stop the SERVER while the client is still connected,
# then stop the client. The order matters twice over: the screenshot is written at the server's shutdown, so a client
# that has already gone would be a photograph of the desktop — which is exactly the bug this script's first draft had.
run_case() {
	local mode="$1"
	local frames="$2"
	local shot="$3"
	local log="$4"
	shift 4

	SOCK="aws-damage-${mode}-$$"
	SHOT="${shot}"
	rm -f "${RUNTIME}/${SOCK}" "${RUNTIME}/${SOCK}.lock" "${SHOT}" "${log}"

	WINDOW_SERVER_DAMAGE_DIAG=1 "${SERVER}" ${SERVER_EXTRA} --socket "${SOCK}" --input none --frames 1000000 \
		--screenshot "${SHOT}" --system-menu-icon hamburger >"${log}" 2>&1 &
	SPID=$!
	for _ in $(seq 1 50); do
		[ -S "${RUNTIME}/${SOCK}" ] && break
		sleep 0.1
	done
	if [ ! -S "${RUNTIME}/${SOCK}" ]; then
		echo "FAIL: the server never created its socket (${mode})" >&2
		cat "${log}" >&2
		exit 1
	fi

	WAYLAND_DISPLAY="${SOCK}" "${CLIENT}" --width "${W}" --height "${H}" --x "${OLD_X}" --y "${OLD_Y}" \
		--colour "${BASE_COLOUR}" --strip "${STRIP_Y0}:${STRIP_Y1}:${STRIP_COLOUR}" \
		--third "${THIRD_COLOUR}" --damage "${mode}" --frames "${frames}" --hold "$@" \
		>/dev/null 2>&1 &
	CPID=$!
	sleep 1.5
	kill -TERM "${SPID}" 2>/dev/null
	wait "${SPID}" 2>/dev/null
	SPID=""
	kill "${CPID}" 2>/dev/null
	wait "${CPID}" 2>/dev/null
	CPID=""

	if [ ! -f "${SHOT}" ]; then
		echo "FAIL: the server wrote no screenshot (${mode})" >&2
		cat "${log}" >&2
		exit 1
	fi
}

check_case() {
	local mode="$1" shot="$2"
	shift 2

	if ! "${PYTHON}" "${CHECKER}" "${shot}" "${mode}" "${BASE_COLOUR}" "${STRIP_COLOUR}" "${THIRD_COLOUR}" \
		"${W}x${H}" "${OLD_X},${OLD_Y}" "$@"; then
		echo "FAIL: the ${mode} frame is wrong" >&2
		FAILED=1
	fi
}

"${PYTHON}" -c 'import PIL' 2>/dev/null || {
	echo "SKIP: Pillow is not installed, and this check reads pixels" >&2
	exit 77
}

echo "1. a client damages a strip, and the frame paints the strip and nothing else"
SHOT_STRIP="${RUNTIME}/agora-damage-strip-$$.ppm"
LOG_STRIP="${RUNTIME}/agora-damage-strip-$$.log"
run_case strip 3 "${SHOT_STRIP}" "${LOG_STRIP}"
check_case strip "${SHOT_STRIP}"
# **THE DIAGNOSTIC'S HALF: the box, and a full redraw that can be accounted for.** The first commit maps the window, so
# the first frame is FULL and must NAME the trigger; every frame after it is the strip's own box.
if ! grep -q "damage: 1 surface(s), box ${W}x$(( STRIP_Y1 - STRIP_Y0 )) at (${STRIP_BOX_AT_X},${STRIP_BOX_AT_Y})" \
	"${LOG_STRIP}"; then
	echo "FAIL: the strip's damage box was not printed as ${W}x$(( STRIP_Y1 - STRIP_Y0 )) at (${STRIP_BOX_AT_X},${STRIP_BOX_AT_Y})" >&2
	grep -E 'damage:' "${LOG_STRIP}" >&2
	FAILED=1
fi
if ! grep -q "damage: FULL (a surface's buffer changed size)" "${LOG_STRIP}"; then
	echo "FAIL: the first frame was not accounted for as a full redraw with a reason" >&2
	grep -E 'damage:' "${LOG_STRIP}" >&2
	FAILED=1
fi
if [ "$(grep -c 'damage: FULL' "${LOG_STRIP}")" -ne 1 ]; then
	echo "FAIL: a strip-only run produced more than one full redraw" >&2
	grep -E 'damage:' "${LOG_STRIP}" >&2
	FAILED=1
fi
cleanup_case

echo "2. a commit that damages NOTHING means the WHOLE buffer"
SHOT_WHOLE="${RUNTIME}/agora-damage-whole-$$.ppm"
LOG_WHOLE="${RUNTIME}/agora-damage-whole-$$.log"
run_case whole 4 "${SHOT_WHOLE}" "${LOG_WHOLE}"
check_case whole "${SHOT_WHOLE}"
if ! grep -q "damage: 1 surface(s), box ${W}x${H} at (${OLD_X},${OLD_Y})" "${LOG_WHOLE}"; then
	echo "FAIL: the whole-buffer commit did not repaint the whole window" >&2
	grep -E 'damage:' "${LOG_WHOLE}" >&2
	FAILED=1
fi
cleanup_case

echo "3. a window that moves repaints BOTH its new place and the ground it left"
SHOT_MOVE="${RUNTIME}/agora-damage-move-$$.ppm"
LOG_MOVE="${RUNTIME}/agora-damage-move-$$.log"
run_case move 4 "${SHOT_MOVE}" "${LOG_MOVE}" --move-to "${NEW_X},${NEW_Y}"
check_case move "${SHOT_MOVE}" "${NEW_X},${NEW_Y}"
if ! grep -q 'damage: FULL' "${LOG_MOVE}"; then
	echo "FAIL: a moved window did not produce a full redraw" >&2
	grep -E 'damage:' "${LOG_MOVE}" >&2
	FAILED=1
fi
cleanup_case

# **AND THE GL RENDERER'S BOX, WHERE ITS SWAPS MAKE ONE MEANINGFUL.** A partial GL frame draws into the back buffer and
# leaves the rest of it alone, which is only correct if the next back buffer begins as the last front buffer did —
# `EGL_BUFFER_PRESERVED`. A driver may refuse (MEASURED: NVIDIA's EGL takes the request and answers
# `EGL_BUFFER_DESTROYED`), and a GBM scanout buffer is a different allocation every frame, so on those the renderer
# composites the WHOLE output every frame — correct, slow, and a full frame that legitimately shows the poison, because
# the buffer really does contain it. So this case is run only when the server says its swaps preserve, and it says so on
# startup rather than leaving the difference to be guessed.
echo "4. and the GL renderer paints the same box, where its swaps preserve what it does not paint"
if [ -z "${DISPLAY:-}" ]; then
	echo "  SKIP: no DISPLAY for the nested backend"
	SKIPPED_GL=1
else
	SERVER_EXTRA="--backend x11 --renderer gl --scale 1"
	SHOT_GL="${RUNTIME}/agora-damage-gl-$$.ppm"
	LOG_GL="${RUNTIME}/agora-damage-gl-$$.log"
	run_case strip 3 "${SHOT_GL}" "${LOG_GL}"
	if ! grep -q "renderer: gl" "${LOG_GL}"; then
		echo "  SKIP: the GL renderer did not start, so there is no second renderer to ask"
		SKIPPED_GL=1
	elif grep -q "gl: a swap preserves" "${LOG_GL}"; then
		check_case strip "${SHOT_GL}"
		if ! grep -q "damage: 1 surface(s), box ${W}x$(( STRIP_Y1 - STRIP_Y0 )) at (${STRIP_BOX_AT_X},${STRIP_BOX_AT_Y})" "${LOG_GL}"; then
			echo "FAIL: the GL renderer did not paint the strip's box" >&2
			grep -E 'damage:' "${LOG_GL}" >&2
			FAILED=1
		fi
	else
		# **THE HONEST SKIP: this driver cannot preserve a back buffer, so the renderer's full frame is the correct
		# answer and there is no box to compare.** The line is printed rather than swallowed, because "GL draws the
		# whole output" is a fact about the session that a reader of the results should know.
		echo "  SKIP: this GL driver destroys the back buffer, so the renderer's frames are FULL (see its own startup line)"
		SKIPPED_GL=1
	fi
	cleanup_case
	SERVER_EXTRA=""
	rm -f "${SHOT_GL}" "${LOG_GL}"
fi

rm -f "${SHOT_STRIP}" "${LOG_STRIP}" "${SHOT_WHOLE}" "${LOG_WHOLE}" "${SHOT_MOVE}" "${LOG_MOVE}"

if [ "${FAILED}" -ne 0 ]; then
	exit 1
fi
echo "damage: all three frames are what the client said they were"
