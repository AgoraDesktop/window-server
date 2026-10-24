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

static void check_dpi(double dpi, double want, const char *what)
{
	double got = aws_scale_from_dpi(dpi);
	int ok = fabs(got - want) < 1e-9;

	printf("%s %-62s %g dpi -> %.6f (want %.6f)\n", ok ? "  ok  " : "  FAIL", what, dpi, got, want);
	if (!ok) {
		failures++;
	}
}

static void check_display(int pixels_wide, int pixels_high, int millimetres, double want, const char *what)
{
	double got = aws_scale_for_display(pixels_wide, pixels_high, millimetres);
	int ok = fabs(got - want) < 1e-9;

	printf("%s %-62s %dx%d on %d mm -> %.6f (want %.6f)\n", ok ? "  ok  " : "  FAIL", what, pixels_wide,
	       pixels_high, millimetres, got, want);
	if (!ok) {
		failures++;
	}
}

static void check_low_resolution(int pixels_high, int want, const char *what)
{
	int got = aws_display_is_low_resolution(pixels_high);
	int ok = got == want;

	printf("%s %-62s height %d -> %s\n", ok ? "  ok  " : "  FAIL", what, pixels_high,
	       got ? "low-resolution" : "measured");
	if (!ok) {
		failures++;
	}
}

static void check_quantized(double scale, double want, const char *what)
{
	double got = aws_scale_quantized(scale);
	int ok = fabs(got - want) < 1e-12;

	printf("%s %-62s %g -> %.9f (want %.9f)\n", ok ? "  ok  " : "  FAIL", what, scale, got, want);
	if (!ok) {
		failures++;
	}
}

static void check_logical(int device, double scale, int want, const char *what)
{
	int got = aws_logical_from_device(device, scale);
	int ok = got == want;

	printf("%s %-62s %d device at %g -> %d (want %d)\n", ok ? "  ok  " : "  FAIL", what, device, scale, got,
	       want);
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

	/* **THE OVERRIDE SPELLED AS DOTS PER INCH IS THE SAME ARITHMETIC.** `--dpi 96` must be the 1.333… above and 72
	 * must be exactly 1, or the two spellings of one instruction would disagree about the same screen. */
	check_dpi(96.0, 96.0 / AWS_POINTS_PER_INCH, "96 dpi as an override is 1.333... device pixels per point");
	check_dpi(72.0, 1.0, "72 dpi as an override is 1 device pixel per point");
	check_dpi(288.0, 4.0, "288 dpi as an override is 4 device pixels per point");

	/* **AND AN OVERRIDE THAT IS NOT A NUMBER A SCREEN COULD HAVE IS REFUSED.** Zero, a negative, NaN and an infinity
	 * would each become a framebuffer of no width or a scale no integer can hold; the caller reports the refusal
	 * rather than starting a session nobody can use. */
	check_dpi(0.0, 0.0, "0 dpi is not an override");
	check_dpi(-96.0, 0.0, "a negative dpi is not an override");
	check_dpi(1.0e9, 0.0, "an absurd dpi does not become a framebuffer");
	check_dpi(NAN, 0.0, "a NaN dpi is not an override");

	/* **THE 1080p RULE IS THE FALLBACK, NOT THE POLICY.** A 1920x1080 panel that reports 531 mm is believed: it is
	 * 91.8 dpi, so a point is 1.276 device pixels and 23 points are 23/72 inch. The 96-dpi answer below is only for
	 * a display whose EDID gives no usable size. */
	check_display(1920, 1080, 531, 1920.0 * 25.4 / 531.0 / AWS_POINTS_PER_INCH,
		      "1080p with a size is measured: 91.8 dpi, not 96");
	check_display(1280, 720, 0, AWS_LOW_RESOLUTION_DPI / AWS_POINTS_PER_INCH,
		      "720p with no EDID falls back to 96 dpi");
	check_display(3840, 1080, 1016, AWS_LOW_RESOLUTION_DPI / AWS_POINTS_PER_INCH,
		      "a twin-head 1080p X screen measures 96 dpi and keeps it");
	check_display(1920, 1080, 40, AWS_LOW_RESOLUTION_DPI / AWS_POINTS_PER_INCH,
		      "a corrupt EDID on a 1080p screen falls back to 96 dpi");

	/* **ABOVE 1080p THE PANEL IS STILL MEASURED**, because that is where a dense laptop panel and a desktop monitor
	 * genuinely differ. 1081 is deliberately the first height on the measured side of the threshold. */
	check_display(2560, 1440, 597, 2560.0 * 25.4 / 597.0 / AWS_POINTS_PER_INCH,
		      "1440p is measured: 1.51 device pixels per point");
	check_display(3840, 2160, 294, 3840.0 * 25.4 / 294.0 / AWS_POINTS_PER_INCH,
		      "4K is still measured: 4.61 device pixels per point");
	check_display(1920, 1081, 531, 1920.0 * 25.4 / 531.0 / AWS_POINTS_PER_INCH,
		      "one line above 1080p is measured again");
	check_display(1920, 0, 531, 1920.0 * 25.4 / 531.0 / AWS_POINTS_PER_INCH,
		      "no mode is not 1080p: it falls through to the measurement");

	check_low_resolution(1080, 1, "1080 is the last low-resolution height");
	check_low_resolution(720, 1, "720p is low-resolution");
	check_low_resolution(1081, 0, "1081 is measured");
	check_low_resolution(2160, 0, "2160 is measured");
	check_low_resolution(0, 0, "a missing mode is not low-resolution");
	check_low_resolution(-1, 0, "a negative mode is not low-resolution");

	/* **AND THE NUMBER THE WIRE CARRIES IS THE NUMBER BOTH ENDS USE.** 1/256 is `wl_fixed`; quantising once is what
	 * keeps a client's `round(points * scale)` and the compositor's placement in agreement, so the blit is 1:1. */
	check_quantized(96.0 / AWS_POINTS_PER_INCH, 341.0 / 256.0, "96 dpi quantises to 341/256");
	check_quantized(1.0, 1.0, "a 1x scale is exact");
	check_quantized(2.0, 2.0, "a 2x scale is exact");
	check_quantized(57.97 / AWS_POINTS_PER_INCH, 206.0 / 256.0, "57.97 dpi quantises to 206/256");
	check_quantized(0.0, 0.0, "no scale stays no scale");
	check_quantized(-1.0, 0.0, "a negative scale is not a scale");

	/* **AND THE INVERSE, WHICH IS WHAT MAKES A CLICK LAND WHERE THE POINTER IS.** A host window reports device pixels;
	 * every coordinate above the backend is logical, and these are the two points the probe in `backend-x11.c`
	 * measured: (200,150) on a 1.332 output is (150,112) in the compositor. */
	check_logical(200, 1.33203125, 150, "device 200 at 1.332 is logical 150");
	check_logical(150, 1.33203125, 112, "device 150 at 1.332 is logical 112");
	check_logical(400, 2.0, 200, "device 400 at 2x is logical 200");
	check_logical(20, 0.8046875, 24, "device 20 at 0.805 is logical 24");
	check_logical(0, 1.0, 0, "the origin is the origin");
	check_logical(-3, 1.0, -3, "a negative position stays negative");
	check_logical(0, 0.0, 0, "no scale means no change");

	printf("check_scale: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
