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

/* **THE LOW-RESOLUTION FALLBACK: 96 DPI WHEN THE DISPLAY CANNOT SAY.** This is NOT a 1080p rule — a measured
 * display is always believed, because a point is a physical unit and 96 dpi is only the conventional size a
 * screen falls back to when its EDID is unusable. `aws_scale_for_display` measures first and uses this answer
 * only when the measurement returned zero; an explicit `--dpi` or `--scale` still overrides everything. */
#define AWS_LOW_RESOLUTION_MAX_HEIGHT 1080
#define AWS_LOW_RESOLUTION_DPI 96.0

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

/* Whether a display's vertical mode is at or below the fallback threshold. Named rather than written out at
 * each backend, because the diagnostics and the policy must agree about which branch produced the scale. */
int aws_display_is_low_resolution(int pixels_high);

/* **THE SCALE A DISPLAY GETS WHEN NOBODY OVERRODE IT: ITS OWN MEASUREMENT, BECAUSE A POINT IS 1/72 INCH.** Above
 * and below every resolution the panel's millimetres decide; the 96 dpi answer is used only when the measurement
 * says nothing AND the mode is 1080p or lower. Zero means the caller keeps its default. */
double aws_scale_for_display(int pixels_wide, int pixels_high, int millimetres_wide);

#endif /* AGORA_WINDOW_SERVER_SCALE_H */
