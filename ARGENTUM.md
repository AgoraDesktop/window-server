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
