/*
 * scale.h — how many device pixels make a logical point.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * THE POINT IS A PHYSICAL UNIT: 1/72 inch. A desktop that keeps that definition makes a 13" panel and a 27"
 * panel show the same SIZE menu bar, the same SIZE window controls and the same SIZE text — only more or fewer
 * of them. That is the whole of this file: a display reports its mode in pixels and its active area in
 * millimetres, and the scale is the dots per inch that follows divided by 72.
 *
 * IT IS A SEPARATE FILE SO THE ARITHMETIC CAN BE TESTED WITHOUT A DISPLAY. The backends that have a display ask
 * it; the unit test asks it directly. See `tests/scale.c`.
 */

#ifndef AGORA_WINDOW_SERVER_SCALE_H
#define AGORA_WINDOW_SERVER_SCALE_H

/* THE DEFINITION OF A POINT, in points per inch. A scale of 2 means two device pixels to a point, which is a
 * 144-dpi display; 1 means 72 dpi, which is a legacy 72-dpi screen and the one case where device and logical
 * pixels are the same size. */
#define AWS_POINTS_PER_INCH 72.0

/* **THE SCALE THAT MAKES 72 POINTS AN INCH, OR ZERO WHEN THE DISPLAY DID NOT SAY.** `pixels_wide` is the mode's
 * horizontal resolution and `millimetres_wide` is the connector's reported active width. A zero answer means the
 * display cannot be believed — no millimetres, an impossible size, or a dots-per-inch outside what a real panel
 * has — and the caller keeps its default of 1 rather than inventing a screen nobody can use.
 *
 * The result is NOT rounded to a whole number: 96 dpi is 1.333… device pixels per point, and a compositor that
 * rounded it to 1 would draw a 96-dpi screen at three-quarters of its physical size. */
double aws_scale_from_physical_size(int pixels_wide, int millimetres_wide);

/* **THE SAME ARITHMETIC FROM THE OTHER END, FOR A PERSON WHO KNOWS THE NUMBER THEY WANT.** `--dpi 96` is a
 * statement about the display, not about points, so it is converted once here (`96 / 72 = 1.333…`) rather than
 * being a second formula in `main`. A non-positive, NaN or unbounded answer means "not a display" and returns 0,
 * which the caller refuses; unlike `aws_scale_from_physical_size` there is no plausibility range of 50–400 dpi,
 * because an override is an instruction rather than an EDID lie — an unusual but sane scale is the person's to
 * choose. */
double aws_scale_from_dpi(double dpi);

/* **THE FALLBACK: 96 DPI WHEN THE DISPLAY CANNOT SAY, AND AT EVERY RESOLUTION — THERE IS NO 1080p RULE.** A
 * measured display is always believed, because a point is a physical unit; this number is only the conventional
 * size a screen falls back to when its EDID is unusable, and a 720p projector that says nothing is no more
 * entitled to it than a 4K television that says nothing. A resolution is a pixel count, not a physical size, so
 * it is not an input to this decision at all — `aws_scale_for_display` is not even handed one. An explicit
 * `--dpi` or `--scale` still overrides everything. */
#define AWS_FALLBACK_DPI 96.0

/* **THE USER'S OWN COMFORT FACTOR, AND IT IS A MULTIPLIER APPLIED LAST — 1.0 IS "SHOW ME THE TRUTH".** Everything
 * above answers "how many device pixels make a point on THIS panel", which is a fact about the hardware. A person
 * who finds a physically honest desktop too small or too large is saying something about their eyes and their
 * distance from the screen, not about the panel — so the answer is a factor that multiplies whatever the display
 * produced, never a second opinion about the display.
 *
 * IT IS APPLIED AFTER THE `--dpi`/`--scale` OVERRIDE TOO, because that override is also a statement about the
 * DISPLAY ("the reported millimetres are wrong"). So the order is: measure or override, then comfort. 1.25 is
 * "everything a quarter larger than the panel's own answer"; 1.0, the default, changes nothing at all. */
#define AWS_DEFAULT_UI_SCALE 1.0

/* **AND AS FAR AS A REFUSAL GOES, 16 IS THE CEILING.** This is not the EDID plausibility range: a factor outside it
 * is not a corrupt size block, it is a person's instruction, and 2 or 3 on a dense laptop panel is an ordinary
 * request. The bound exists only because the factor multiplies a framebuffer size, and a number large enough to
 * overflow that arithmetic is a typo rather than a preference. Zero, negative and NaN are refused for the same
 * reason `--dpi 0` is: "no multiplier" and "a multiplier of nothing" must not be the same answer. */
#define AWS_MAX_UI_SCALE 16.0

/* **THE FACTOR AS THE REST OF THIS FILE MAY USE IT: 1 WHEN NOBODY SAID, AND THE NUMBER WHEN THEY DID.** A zero, a
 * negative, a NaN or an infinity all mean "no instruction" here rather than "multiply by that", because this
 * function is called on the path that sizes a framebuffer — the one place a silently unusable number is worse than
 * a default. Whether the caller should have refused the value beforehand is a separate question, answered by
 * `aws_ui_scale_is_plausible`. */
double aws_ui_scale_normalized(double ui_scale);

/* Whether a factor is one a session can be started with: positive, finite, and at most `AWS_MAX_UI_SCALE`. This is
 * what the command line asks before it hands a number over, so `--ui-scale 0` is refused loudly instead of being
 * quietly read as 1. */
int aws_ui_scale_is_plausible(double ui_scale);

/* **THE MULTIPLICATION ITSELF, AND THE ONE PLACE THE UI FACTOR MEETS THE WIRE.** `aws_scale_with_ui` quantises its
 * input first (so passing an already-quantised scale is exact, not approximate), multiplies, and quantises the
 * product — ONE rounding at the end, which is what a client is told and what the compositor blits 1:1. With a
 * factor of 1 the answer is exactly `aws_scale_quantized(scale)`, which is what makes the default a no-op rather
 * than a nearly-no-op. A scale of zero stays zero ("not a display"). */
double aws_scale_with_ui(double scale, double ui_scale);

/* **AND THE SCALE THE WIRE CAN CARRY EXACTLY: 1/256, WHICH IS `wl_fixed`.** A fractional output scale is only
 * resolution independence if the client and the compositor agree on it to the last bit: the client sizes its buffer
 * with `round(points * scale)` and the compositor places that buffer 1:1, and a scale that differed between them by
 * even 1/1000 would turn the blit into a resample. So the compositor quantises its own scale ONCE, before the
 * framebuffer is sized or a client is told anything, and every later use — its furniture, its placement, the wire —
 * is the same number. Zero or a negative scale stays zero ("not a display"). */
double aws_scale_quantized(double scale);

/* **THE INVERSE, FOR AN INPUT EVENT THAT ARRIVES IN DEVICE PIXELS.** A pointer comes from a device whose coordinates
 * are pixels, while every coordinate above the backend — a window's origin, the hit test, what a client is told — is
 * LOGICAL. `aws_logical_from_device` is the exact inverse of the placement rounding (`round(x * scale)`), and it
 * TRUNCATES on purpose: a device pixel belongs to the logical pixel it starts in, which is what makes two adjacent
 * windows tile at their shared edge instead of one pixel overlapping the other. */
int aws_logical_from_device(int device, double scale);

/* **THE SCALE A DISPLAY GETS WHEN NOBODY OVERRODE IT: ITS OWN MEASUREMENT, BECAUSE A POINT IS 1/72 INCH.** The
 * panel's millimetres decide, at every resolution; `AWS_FALLBACK_DPI` answers only when the measurement says
 * nothing. `pixels_high` is deliberately NOT a parameter: it used to be one, because the fallback was keyed on the
 * mode's height, and dropping it is what keeps a resolution from becoming part of the decision again. Zero means the
 * mode is not a display at all, and the caller keeps its own default. */
double aws_scale_for_display(int pixels_wide, int millimetres_wide);

#endif /* AGORA_WINDOW_SERVER_SCALE_H */
