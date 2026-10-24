/*
 * scale.c — 72 points must be one inch, on any panel.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE RULE, STATED ONCE: a logical point is 1/72 inch, so a display of D dots per inch needs `D / 72` device
 * pixels per point. The cases below are real panels and the ways EDID lies, and the tolerance is small enough that
 * a rounded-to-1 or rounded-to-2 scale cannot pass.
 */

#include "scale.h"

#include <math.h>
#include <stdio.h>

static int failures;

static void check(int pixels, int millimetres, double want, const char *what)
{
	double got = aws_scale_from_physical_size(pixels, millimetres);
	int ok = fabs(got - want) < 1e-9;

	printf("%s %-62s %d px on %d mm -> %.6f (want %.6f)\n", ok ? "  ok  " : "  FAIL", what, pixels,
	       millimetres, got, want);
	if (!ok) {
		failures++;
	}
}

int main(void)
{
	/* A 24" 1080p monitor: 1920 px across 531 mm is 91.8 dpi, so a point is 1.276 device pixels. */
	check(1920, 531, 1920.0 * 25.4 / 531.0 / AWS_POINTS_PER_INCH, "24-inch 1080p is 1.28 device pixels per point");

	/* A 13" 4K laptop panel: 3840 px across 294 mm is 331.8 dpi, so a point is 4.61. This is the case the old
	 * two-threshold rule answered with 2, which made every window half the physical size it should be. */
	check(3840, 294, 3840.0 * 25.4 / 294.0 / AWS_POINTS_PER_INCH, "13-inch 4K is 4.61 device pixels per point");

	/* 96 dpi exactly: the scale is fractional and must not be rounded to a whole number. */
	check(1920, 508, 96.0 / AWS_POINTS_PER_INCH, "96 dpi is 1.333... device pixels per point");

	/* 72 dpi exactly: the one display where a point and a pixel are the same size. */
	check(1024, 361, 1024.0 * 25.4 / 361.0 / AWS_POINTS_PER_INCH, "72 dpi is 1 device pixel per point");

	/* **AND THE WAYS EDID LIES.** No millimetres, an impossible width, a denser-than-real answer, and a
	 * projector-sized panel whose points would be smaller than its pixels: each is "I did not say", which the
	 * caller reads as scale 1 rather than as a screen nobody can use. */
	check(1920, 0, 0.0, "a connector that reports no millimetres says nothing");
	check(1920, 40, 0.0, "a 4 cm wide display is not a display");
	check(3840, 160, 0.0, "609 dpi is a corrupt EDID, not a panel");
	check(1920, 1600, 0.0, "a 30 dpi projection is below the plausible range");
	check(-1920, 531, 0.0, "a negative mode says nothing");
	check(0, 531, 0.0, "a zero mode says nothing");

	printf("check_scale: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
