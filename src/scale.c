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

/* **A PERSON'S COMFORT FACTOR IS A NUMBER OR IT IS NOTHING; IT IS NEVER A GUESS.** Zero and a negative say nothing,
 * and NaN fails the same comparison, which is why the test is written as `!(ui > 0.0)`. The ceiling is the only
 * bound, and it is there for the framebuffer arithmetic rather than for taste — see `AWS_MAX_UI_SCALE`. The
 * three ways of writing "no instruction" therefore all answer 1, which is the default and a no-op. */
double aws_ui_scale_normalized(double ui_scale)
{
	if (!(ui_scale > 0.0)) {
		return AWS_DEFAULT_UI_SCALE;
	}
	if (!(ui_scale <= AWS_MAX_UI_SCALE)) {
		/* NaN already answered above, so this is an infinity or a number past the ceiling. Neither is a factor a
		 * session can be sized with; the callers that REFUSE such a value have said so already, and the callers
		 * that merely size things get the default instead of an overflow. */
		return AWS_DEFAULT_UI_SCALE;
	}
	return ui_scale;
}

int aws_ui_scale_is_plausible(double ui_scale)
{
	return ui_scale > 0.0 && ui_scale <= AWS_MAX_UI_SCALE;
}

/* **ONE ROUNDING, AT THE END — AND A FACTOR OF 1 MUST NOT ROUND AT ALL.** The input is quantised because that is
 * what the display's answer already was ("the scale everything else produced" is the number a client would have
 * been told), and the product is quantised because that is the number a client WILL be told and the number the
 * compositor blits 1:1. Quantising an already-quantised value is exact by construction, so folding this in at each
 * place a scale is decided cannot drift. */
double aws_scale_with_ui(double scale, double ui_scale)
{
	double base = aws_scale_quantized(scale);

	if (!(base > 0.0)) {
		return 0.0;
	}
	return aws_scale_quantized(base * aws_ui_scale_normalized(ui_scale));
}

/* **THE MEASUREMENT WINS; 96 DPI IS ONLY THE FALLBACK WHEN THERE IS NO MEASUREMENT — AND THE MODE IS NOT
 * CONSULTED.** A 1080p panel that reports its size is drawn at its real dots per inch, because that is what makes
 * 23 points 23/72 inch; a display of ANY size that cannot say gets the conventional 96 dpi, because a corrupt
 * EDID, a projector and a TV are the same problem whether their mode is 1280x720 or 3840x2160. The mode is not a
 * parameter, so a resolution rule cannot come back by accident. A zero-width mode is not a display and still
 * answers zero, which the caller reads as "keep your default". */
double aws_scale_for_display(int pixels_wide, int millimetres_wide)
{
	double measured;

	if (pixels_wide <= 0) {
		return 0.0;
	}
	measured = aws_scale_from_physical_size(pixels_wide, millimetres_wide);
	if (measured > 0.0) {
		return measured;
	}
	return AWS_FALLBACK_DPI / AWS_POINTS_PER_INCH;
}
