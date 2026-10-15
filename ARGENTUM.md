# Argentum

**Argentum is the design language Agora's desktop is drawn in.** This file is where its numbers live, because they
are the design's and not any one component's: a menu bar and the controls beneath it have to agree, and the way they
come to differ by four counts is each of them choosing its own greys.

## Where these numbers come from

**Every value below was MEASURED from Argentum's own sample** — `Argentum Mockup.png` (1920×1084) — and not chosen
here. The mockup is a *representative sample* of the language rather than a specification of a screen, so the values
are what it uses rather than a layout to reproduce pixel for pixel.

| role | value | sampled from |
|---|---|---|
| the menu bar's face | `#EBEBEB` (235,235,235) | the bar's body, y=0..24 |
| the bar's ink | `#000000` | the darkest pixel in the bar's text |
| the accent | `#A49AFF` (164,154,255) | the bar's highlighted item, x=168..220 |
| a window's face | `#F9F9F9` (249,249,249) | a window's body |
| a second light surface | `#FFFFFF` | a lighter panel |
| the wallpaper | **a gradient** | see below |

**The wallpaper is the one thing that is not one colour.** The sample runs from a warm amber at the left edge —
`(72,36,14)` and `(108,55,29)` — to near-black at the bottom corners, `(18,14,5)` and `(23,16,6)`. **THE SERVER DOES
NOT FILL ANY OF THEM: it fills `#A8DED9` (168,222,217), a light blue-green aqua, BY REQUEST** — a departure from the
mockup and recorded as one, so the numbers above stay the record of what the sample shows rather than a description
of what the desktop does. The fill is still one colour rather than the sample's gradient, and a vertical ramp is
still the honest next step.

## The scroller

The sample draws a scroller along a scroll view's **right and bottom** edges, and every part of it is custom drawing
rather than a toolkit's widget — so these numbers are the design, and not a reading of some other toolkit's:

| role | value | sampled from |
|---|---|---|
| the groove the knob runs in | `#D9D9D9` (217,217,217) | the track, x=764..780 |
| the knob's face | `#FFFFFF` | the knob's body, y=225..266 |
| the knob's edge, and its dimples | `#BCBCBC` (188,188,188) | the knob's outline, and the darkest point inside a dimple |
| an arrow's ink | `#000000` | the glyph, x=770..775 |
| the bar's thickness | **17 px** | the track's width — this tree builds scrollers 15 wide |
| an arrow | **6 wide, 4 tall** | the glyph's bounding box, and it is WIDER THAN IT IS TALL |

**AND `#BCBCBC` IS EVERY BORDER IN THE TIER, NOT A SCROLLER'S OWN.** 188 is what `+controlDarkShadowColor` answers, so
a knob's outline, its three dimples, a text field's edge, a button's bezel and a slider's track are ONE value — the
sample uses it for all of them, which is what says it is a value of the design rather than a grey each control picks
for itself. It was 0.55 in the tier until the sample was measured.

The knob carries **three RINGS down its centre**, 9 px apart. They are rings and not filled dots because that is what
the sample shows and not an artifact of the export: the darkest value in one is 188 while its middle is 255, so the
knob's own white shows through the centre. The hole is only a couple of points across, which is why it survived a
downscaled export at all.

**The sample is a BITMAP EXPORTED FROM AFFINITY DESIGNER, which is the one thing to know before reading any edge in
it.** Every boundary in the file is blended — the arrow's sides run 194, 212, 229, 243 and 128, 181 through a 255
ground — and that is what rasterizing a curve does, not a statement about how sharp the design wants its arrow. So
the SHAPES and the GREYS above are the design and the edge values are the export, which is also why the arrow's box
is measured a pixel generous at each side.

**And the knob is BRIGHTER THAN ITS GROUND, which is the one place the sample inverts the rule the rest of this file
is about.** Everywhere else a raised thing is lighter than the window beneath it; a scroller's ground is the GROOVE
rather than the window, so the knob takes the white and the groove takes the grey — and `#D9D9D9` is still darker
than all three of the raised faces above (`#F9F9F9`, `#EBEBEB`, white), which is what keeps the knob reading as
raised even with four surfaces in play.

## The pop-up button

**The sample draws this control as a FIELD with a chip in it**, not as a raised button with an arrow beside the words:
a white rectangle with a one-point border — which is what makes it read as a text field — holding an accent-filled
chip at its right end.

| role | value | sampled from |
|---|---|---|
| the field's face | `#FFFFFF` | the row's interior |
| the field's border | `#BCBCBC` (188,188,188) | x=919, x=1069 and y=398 of the row, one point each |
| the field's corners | **4 px** | the radius a text field also takes |
| the chip | **20 x 20**, inset **5** from the field's right border | the accent region at x=1045..1064, y=402..421 |
| the chip's face | `#A49AFF` | the platform's accent |
| the chip's corners | **4 px** | the same radius again |
| the chevrons | **10 wide, 5 tall, 2 apart**, in the field's white | the knocked-out triangles, x=1050..1059 |
| the field's height | **26 px** | y=398..423 — this tree's controls are 23 |

**AND THE TWO CHEVRONS ARE THIS CONTROL'S OWN SIGN.** The `NSComboBox` beside it in the same gallery is the same field
with ONE triangle: a pop-up chooses both ways through its list and a combo box opens one. The label on each row of
that gallery names the control drawn there, which is how the pair is told apart.

**THE ARROWS ARE WHITE BECAUSE THEY ARE HOLES, NOT MARKS** — they are knocked out of the chip rather than drawn on it,
which is why the chip's own fill is the accent and the triangles take the field's white.

**AND `#BCBCBC` IS THE TIER'S BORDER GREY TOO** — `+controlDarkShadowColor` — so this field, a button beside it and a
scroller's knob are all outlined in one value, which is the relationship `NSText` argues for where it draws the same
face for a text field.

## What the sample also says about LAYOUT

Measured, and worth keeping because two of them are things this desktop does not do yet:

- **the menu bar is 25 px tall** — the server reserves 24, so the bar is one pixel shorter than the design's
- **eight items on the left**: the System menu's glyph (18 px wide), the application's own menu (86 px), and six more
- **two items on the right** — the status area — at x=1735..1817 and x=1838..1903
- **the application menu's text is 15 px tall** (ink rows 7..21); this tree sets it at 13 px

## And two things the sample says are the SAME colour, which is the point of writing it down

**The accent is used for every selection in the sample** — the highlighted menu bar item, a list selection, a
button — and not only for the bar. A highlight is the same statement wherever it is made, so it is one colour, and
it is why `APPKIT_MENU_HIGHLIGHT_*` and the server's furniture cannot be chosen separately.

**And the bar's face is the surface an application's own menus are drawn on.** The server owns the strip and the
client owns its titles, and there is exactly one value for what they are both painted with — told to the client
over `agora_shell`'s `menu_bar_area` event rather than copied into it. A client that picked a colour which looked
close is how the seam comes back.
