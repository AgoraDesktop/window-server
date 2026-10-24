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

#endif /* AGORA_WINDOW_SERVER_SCALE_H */
