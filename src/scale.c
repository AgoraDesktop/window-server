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
