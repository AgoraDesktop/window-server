/*
 * scale.c — the display's dots per inch, expressed as device pixels per logical point.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ONE FORMULA AND TWO GUARDS. The formula is `dpi / 72`, where `dpi = pixels * 25.4 / millimetres`; it is what
 * makes a point 1/72 inch on any panel. The guards exist because the millimetres come from EDID, and EDID lies:
 * a connector that reports 0 mm has said nothing, and one that reports 160 mm for a 27" panel has said something
 * absurd. Both are treated as "no answer" rather than as a screen with an 8x scale, because a wrong scale is not
 * a cosmetic problem — it resizes every window and can make a session unusable.
 */

#include "scale.h"

/* **WHAT A REAL DISPLAY'S DOTS PER INCH CAN BE.** The floor is below any monitor and above a projector's big
 * throw; the ceiling is above the densest laptop panel (a 13" 4K is about 340) and below the nonsense a broken
 * EDID produces. A value outside this range is not a display description, it is a corrupted one. */
#define AWS_MIN_PLAUSIBLE_DPI 50.0
#define AWS_MAX_PLAUSIBLE_DPI 400.0

/* AND A DISPLAY NARROWER THAN THIS IS NOT A DISPLAY. 50 mm is two inches; the narrowest real panel is a phone. */
#define AWS_MIN_PLAUSIBLE_MILLIMETRES 50

double aws_scale_from_physical_size(int pixels_wide, int millimetres_wide)
{
	double dpi;

	if (pixels_wide <= 0 || millimetres_wide < AWS_MIN_PLAUSIBLE_MILLIMETRES) {
		return 0.0;
	}
	dpi = (double)pixels_wide * 25.4 / (double)millimetres_wide;
	if (dpi < AWS_MIN_PLAUSIBLE_DPI || dpi > AWS_MAX_PLAUSIBLE_DPI) {
		return 0.0;
	}
	return dpi / AWS_POINTS_PER_INCH;
}

/* **AN OVERRIDE'S ONLY GUARD IS THAT IT MUST BE A NUMBER A FRAMEBUFFER CAN HOLD.** Zero and a negative say
 * nothing, and an infinity would become a scale no integer can express; NaN fails the same comparison, which is
 * why the test is written as `!(dpi > 0.0)` rather than `dpi <= 0.0`. Everything else is accepted, including a
 * dots-per-inch outside the EDID range above: those guards protect against a corrupt size block, and a person
 * asking for 40 dpi is not a corrupt size block. */
double aws_scale_from_dpi(double dpi)
{
	if (!(dpi > 0.0) || !(dpi < 1.0e6)) {
		return 0.0;
	}
	return dpi / AWS_POINTS_PER_INCH;
}

int aws_display_is_low_resolution(int pixels_high)
{
	return pixels_high > 0 && pixels_high <= AWS_LOW_RESOLUTION_MAX_HEIGHT;
}

int aws_logical_from_device(int device, double scale)
{
	double logical;
	long whole;

	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	logical = (double)device / scale;
	whole = (long)logical;
	/* FLOOR, NOT TRUNCATE, so a pointer dragged off the left edge stays negative instead of being rounded up
	 * into the screen. For the ordinary non-negative case the two are the same. */
	if ((double)whole > logical) {
		whole--;
	}
	return (int)whole;
}

double aws_scale_quantized(double scale)
{
	/* ROUND, NOT TRUNCATE: truncating would make 1.3333 into 1.328 and lose a hundredth of the screen. The integer
	 * arithmetic is the point — a double multiply-and-divide could round a different way from `wl_fixed_from_double`
	 * in the protocol library, and the client would then disagree by one unit. */
	if (!(scale > 0.0) || !(scale < 1.0e6)) {
		return 0.0;
	}
	return (double)((long)(scale * 256.0 + 0.5)) / 256.0;
}

/* **THE MEASUREMENT WINS; 96 DPI IS ONLY THE FALLBACK WHEN THERE IS NO MEASUREMENT.** A 1080p panel that reports its
 * size is drawn at its real dots per inch, because that is what makes 23 points 23/72 inch. The 96-dpi answer is for
 * the display that CANNOT say — a corrupt EDID, a projector, a TV — where a conventional size is better than a
 * guessed one. This ordering is the whole of resolution independence on this side of the wire. */
double aws_scale_for_display(int pixels_wide, int pixels_high, int millimetres_wide)
{
	double measured = aws_scale_from_physical_size(pixels_wide, millimetres_wide);

	if (measured > 0.0) {
		return measured;
	}
	if (aws_display_is_low_resolution(pixels_high)) {
		return AWS_LOW_RESOLUTION_DPI / AWS_POINTS_PER_INCH;
	}
	return 0.0;
}
