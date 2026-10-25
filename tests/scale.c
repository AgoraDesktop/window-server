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

static void check_display(int pixels_wide, int millimetres, double want, const char *what)
{
	double got = aws_scale_for_display(pixels_wide, millimetres);
	int ok = fabs(got - want) < 1e-9;

	printf("%s %-62s %d px on %d mm -> %.6f (want %.6f)\n", ok ? "  ok  " : "  FAIL", what, pixels_wide,
	       millimetres, got, want);
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

static void check_normalized(double ui, double want, const char *what)
{
	double got = aws_ui_scale_normalized(ui);
	int ok = fabs(got - want) < 1e-12;

	printf("%s %-62s %g -> %.9f (want %.9f)\n", ok ? "  ok  " : "  FAIL", what, ui, got, want);
	if (!ok) {
		failures++;
	}
}

static void check_plausible(double ui, int want, const char *what)
{
	int got = aws_ui_scale_is_plausible(ui);
	int ok = got == want;

	printf("%s %-62s %g -> %s\n", ok ? "  ok  " : "  FAIL", what, ui, got ? "accepted" : "refused");
	if (!ok) {
		failures++;
	}
}

static void check_with_ui(double scale, double ui, double want, const char *what)
{
	double got = aws_scale_with_ui(scale, ui);
	int ok = fabs(got - want) < 1e-12;

	printf("%s %-62s %g x %g -> %.9f (want %.9f)\n", ok ? "  ok  " : "  FAIL", what, scale, ui, got, want);
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

	/* **THE 96-DPI FALLBACK IS NOT A RESOLUTION POLICY.** A 1920x1080 panel that reports 531 mm is believed: it is
	 * 91.8 dpi, so a point is 1.276 device pixels and 23 points are 23/72 inch. The 96-dpi answer is only for a
	 * display whose EDID gives no usable size, and the mode is not asked about it — the resolution is not even a
	 * parameter, so a small display and a large one get the same conventional answer. */
	check_display(1920, 531, 1920.0 * 25.4 / 531.0 / AWS_POINTS_PER_INCH,
		      "1080p with a size is measured: 91.8 dpi, not 96");
	check_display(1280, 0, AWS_FALLBACK_DPI / AWS_POINTS_PER_INCH, "720p with no EDID falls back to 96 dpi");
	check_display(3840, 1016, AWS_FALLBACK_DPI / AWS_POINTS_PER_INCH,
		      "a twin-head 1080p X screen measures 96 dpi and keeps it");
	check_display(1920, 40, AWS_FALLBACK_DPI / AWS_POINTS_PER_INCH,
		      "a corrupt EDID on a 1080p screen falls back to 96 dpi");

	/* **AND A DISPLAY THAT SAYS NOTHING GETS THE SAME ANSWER WHATEVER ITS MODE IS**, because a resolution is a pixel
	 * count and not a physical size: a 4K television that cannot describe itself is not told to draw its desktop at
	 * a quarter of the size, and nothing about 1080p is special. */
	check_display(3840, 0, AWS_FALLBACK_DPI / AWS_POINTS_PER_INCH,
		      "4K with no EDID gets 96 dpi, not scale 1");
	check_display(3840, 160, AWS_FALLBACK_DPI / AWS_POINTS_PER_INCH,
		      "4K with a corrupt EDID gets the same fallback");

	/* **THE PANEL IS STILL MEASURED WHERE A DENSE LAPTOP AND A DESKTOP MONITOR GENUINELY DIFFER.** */
	check_display(2560, 597, 2560.0 * 25.4 / 597.0 / AWS_POINTS_PER_INCH,
		      "1440p is measured: 1.51 device pixels per point");
	check_display(3840, 294, 3840.0 * 25.4 / 294.0 / AWS_POINTS_PER_INCH,
		      "4K is measured: 4.61 device pixels per point");
	check_display(0, 531, 0.0, "a zero-width mode is not a display, so not a 96 dpi one either");

	/* **AND THE NUMBER THE WIRE CARRIES IS THE NUMBER BOTH ENDS USE.** 1/256 is `wl_fixed`; quantising once is what
	 * keeps a client's `round(points * scale)` and the compositor's placement in agreement, so the blit is 1:1. */
	check_quantized(96.0 / AWS_POINTS_PER_INCH, 341.0 / 256.0, "96 dpi quantises to 341/256");
	check_quantized(1.0, 1.0, "a 1x scale is exact");
	check_quantized(2.0, 2.0, "a 2x scale is exact");
	check_quantized(57.97 / AWS_POINTS_PER_INCH, 206.0 / 256.0, "57.97 dpi quantises to 206/256");
	check_quantized(0.0, 0.0, "no scale stays no scale");
	check_quantized(-1.0, 0.0, "a negative scale is not a scale");

	/* **AND THE USER'S COMFORT FACTOR, WHICH MULTIPLIES WHATEVER THE DISPLAY SAID AND NOTHING ELSE.** The rule this
	 * block exists to pin down is that 1.0 is a NO-OP — not "approximately", exactly — because every session that
	 * existed before the factor did is a session at 1.0. Then: 2 x 1.25 is 2.5, a 96-dpi panel (1.33203125 after
	 * quantisation) x 1.25 is 1.6640625, and a factor BELOW 1 shrinks. The product is quantised once at the end, so
	 * the number here is the number a client is told and the compositor blits 1:1. */
	check_with_ui(2.0, AWS_DEFAULT_UI_SCALE, 2.0, "a factor of 1 changes nothing at 2x");
	check_with_ui(1.33203125, AWS_DEFAULT_UI_SCALE, 1.33203125, "a factor of 1 changes nothing at 1.332");
	check_with_ui(96.0 / AWS_POINTS_PER_INCH, AWS_DEFAULT_UI_SCALE, 341.0 / 256.0,
		      "a factor of 1 on a 96-dpi panel is the same 341/256");
	check_with_ui(2.0, 1.25, 2.5, "a quarter more than 2x is 2.5x");
	check_with_ui(1.33203125, 1.25, 426.0 / 256.0, "96 dpi at 1.25 is 426/256, not a rounded 1.664");
	check_with_ui(2.0, 0.75, 1.5, "three quarters of 2x is 1.5x");
	check_with_ui(1.0, 1.5, 1.5, "a headless output at 1x times 1.5 is a 1.5x screen");
	check_with_ui(4.611111, 2.0, 9.21875, "a dense panel may be made larger still");
	check_with_ui(0.0, 2.0, 0.0, "no display scale stays no display scale, whatever the comfort factor");
	/* An unusable factor is the DEFAULT here rather than a product: this is the path a framebuffer is sized on, and
	 * `--ui-scale 0` is refused before it ever reaches this function. */
	check_with_ui(2.0, 0.0, 2.0, "a factor of zero falls back to 1, not to nothing");
	check_with_ui(2.0, -1.0, 2.0, "a negative factor falls back to 1");
	check_with_ui(2.0, NAN, 2.0, "a NaN factor falls back to 1");
	check_with_ui(2.0, 1.0e9, 2.0, "a factor past the ceiling falls back to 1");

	/* AND WHAT THE COMMAND LINE REFUSES, SO THAT THE FALLBACK ABOVE IS NOT HOW A TYPO IS HANDLED: only a positive
	 * number up to the ceiling is an instruction. 1.25 and 2 are ordinary requests; 0 is not a multiplier. */
	check_normalized(1.25, 1.25, "a quarter larger is itself");
	check_normalized(1.0, 1.0, "the default is itself");
	check_normalized(0.0, 1.0, "zero reads as no instruction here");
	check_normalized(-2.0, 1.0, "a negative reads as no instruction here");
	check_normalized(NAN, 1.0, "a NaN reads as no instruction here");
	check_plausible(1.0, 1, "1 is an instruction");
	check_plausible(1.25, 1, "1.25 is an instruction");
	check_plausible(0.5, 1, "half size is an unfriendly but legal request");
	check_plausible(AWS_MAX_UI_SCALE, 1, "the ceiling itself is legal");
	check_plausible(0.0, 0, "zero is refused rather than read as the default");
	check_plausible(-1.25, 0, "a negative factor is refused");
	check_plausible(NAN, 0, "a NaN factor is refused");
	check_plausible(AWS_MAX_UI_SCALE * 2.0, 0, "past the ceiling is refused, because the framebuffer arithmetic");

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
