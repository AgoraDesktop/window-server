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

**AND THE FIELD'S FACE IS ONE OF TWO VALUES HERE THIS TIER DOES NOT FOLLOW.** The sample paints `#FFFFFF` on a `#F9F9F9`
window; this tier paints every control the other way — a face slightly DARKER than the surface under it, and a disabled
one a step below that — so a pop-up's field takes the face the push button beside it takes and the two AGREE. The
SHAPE is the sample's and the FILL is the tier's, and it is a departure rather than a reading.

**AND THE TITLE'S MARGIN IS THE OTHER.** MEASURED, the sample's label begins two points inside the field's border —
the ink starts at x=921 with the border at x=919 — and this tier begins it four, which is `NS_CELL_CONTENT_GAP`: the
SAME margin a checkbox leaves between its box and its label, named once in `NSCell.h` so the two controls cannot come
to disagree. A pop-up sitting above a checkbox in one window is where two answers to "how far in do the words begin"
are visible at once, and the tier takes its own controls' answer. THE FIELD STILL READS AS A FIELD because of its shape
— one border, radius four — and not because its first letter is two points rather than four from the edge.

**AND THE TWO CHEVRONS ARE THIS CONTROL'S OWN SIGN.** The `NSComboBox` beside it in the same gallery is the same field
with ONE triangle: a pop-up chooses both ways through its list and a combo box opens one. The label on each row of
that gallery names the control drawn there, which is how the pair is told apart.

**THE ARROWS ARE WHITE BECAUSE THEY ARE HOLES, NOT MARKS** — they are knocked out of the chip rather than drawn on it,
which is why the chip's own fill is the accent and the triangles take the field's white.

**AND `#BCBCBC` IS THE TIER'S BORDER GREY TOO** — `+controlDarkShadowColor` — so this field, a button beside it and a
scroller's knob are all outlined in one value, which is the relationship `NSText` argues for where it draws the same
face for a text field.

## The combo box, which is the same field with ONE triangle

**The sample draws this control on the row below the pop-up's and draws it as the SAME CONTROL**, which is the most
useful thing in that gallery: one field, one chip, and one mark between them that differs.

| | the pop-up's chip | the combo box's chip |
|---|---|---|
| the chip | 20 x 20 at x=1045..1064 | **identical** — x=1045..1064 |
| white runs down the centre column | **two**: y=405..409 and y=414..418 | **one**: y=447..451 |
| the widest white row | at each run's BOTTOM | at the run's **TOP** |
| what that is | an up triangle above a down one | a single down triangle |

The centre column is the measurement that settles it, because it needs no inference: two runs of white mean two
triangles knocked out of the accent and one run means one. And the widest row's POSITION says which way the single one
points — a down triangle is widest at its top and an up triangle at its bottom — so "down" is read off the ink rather
than assumed from the arrow's usual direction.

**AND THE DIFFERENCE IS A SENTENCE ABOUT BEHAVIOUR RATHER THAN A DECORATION.** A pop-up chooses both ways through its
list, so it carries both arrows; a combo box opens one list and leaves the choosing to a person who may also type, so
it carries the one that says there is a list down here. Neither arrow is this tier's invention and neither is
interchangeable: the pair is the sample's own way of telling two controls apart at a glance, which is why the drawing
demo puts them side by side on one row rather than on two.

**AND A FIELD'S PADDING IS FOUR POINTS ON ALL FOUR SIDES, WHERE THE SAMPLE'S IS TWO.** MEASURED on the sample, its
field label begins two points inside the border; that is where this tier began, and the field beside the others read as
crowded. Four is the design owner's correction, on the same footing as the prelight's colour and the pop-up's fill —
the sample stays the record of what IT shows, and the departure is written here rather than folded into the reading.
IT IS ALSO ONE NUMBER NOW: the view that draws a field (`NSText`) and the cell that draws one (`NSTextFieldCell`) each
had their own, and they disagreed on the vertical axis while agreeing across it by coincidence. Both read
`NS_TEXT_INSET`, and `NS_CELL_CONTENT_GAP` keeps its own job — the margin between a control's MARK and its words, which
is a checkbox's box and its label or a pop-up's arrow and its title. A FIELD has no mark.

**AND IT CAN BE TYPED INTO, WHICH IS THE ONE THING THE FIRST SLICE OF IT COULD NOT DO.** Apple builds an
`NSComboBoxCell` on an `NSTextFieldCell`, and that base is where a combo box's string, its selection and its edit
session come from; this tier had no such class, so the cell started at `NSActionCell` and its header recorded the row.
The field cell now exists — it owns the face, the string and the three doors of an edit — and the combo box derives
from it, so what is left in the combo box's own file is what is actually a combo box's: the list, the chip, the fill a
control's field takes, and one measurement that stops its text at the chip.

**AND THE CHIP OPENS THE LIST, WHICH IS THE ONE THING IT IS FOR.** A press on the chip opens the list of values; a
press anywhere else in the field begins an edit. That split is the control's whole notion of a press, and MEASURED on
the demonstration the chip did nothing at all until it existed — the ink was drawn and the list behind it was not.

**AND BOTH THIS CONTROL AND THE POP-UP RAISE THEIR WINDOW AT THE CONTROL'S OWN LEFT EDGE.** The sample's pair sit on
consecutive rows with their fields starting at the same x, and a list that opened under the arrow rather than under the
field would break the line the two controls are drawn on. `NSMenu`'s placement puts its window at `location.x` —
measured in its own arithmetic — so the anchor a control hands over IS the alignment, and it is now named once in
`NSCell` for both controls rather than computed in each: the anchor was in two files, and the combo box's had drifted
to its CHIP.

**AND THE LIST IS DRAWN AS A MENU, WHICH IS A DEPARTURE WORTH NAMING — AND HALF PAID FOR.** Apple's combo box opens a
borderless LIST WINDOW of its own; this tier opens the session `NSMenu` already has — the same one the pop-up beside it
raises — and builds the rows from the cell's objects on the way in. What that buys is a session that starts, tracks, is
dismissed and sends a pick, none of which had to be written twice.

**THE ROW THE BOX HOLDS IS NOW MARKED, WHICH WAS THE HALF A PERSON ACTUALLY MISSES.** MEASURED, the list used to open
with every row looking alike, so the one question a combo box's list exists to answer — which row am I on — had to be
answered by reading the whole list and comparing it with the field. The held row now wears the same chip the pointer
wears, because a chip is what this session's highlight IS (`NSMenuView -drawRect:` owns its shape, colour and inset) and
a second kind of mark would be one session drawing two vocabularies for one question. `NSMenuView` gained a door a
subclass can answer about a row the pointer is not on, `NSMenu` gained the ability to say which view draws its rows, and
the combo box's own view answers it from the item's state — the menu's own way of saying "this is the current choice".

**AND THE ROW IS A LIST'S NOW: NO CHECK-MARK GUTTER, NO SHORTCUT COLUMN, AND THE MARK FILLS IT.** A menu lays a row
out in four strips — the state image's gutter, the image, the title, the key equivalent — and a list needs one of them,
so the tier has a row cell of its own that gives the title the whole row and inks nothing in the two columns a list
does not have. The mark became the whole row rather than a chip inside it, because that is the difference between a row
and a menu item. NEITHER CHANGE TOUCHES A MENU: the doors that ask (`-cg_newItemCell`, `-cg_markRectForRow:`) answer
the menu's own answers until a subclass says otherwise, and the probe asserts that a plain menu view still makes the
menu's cell and still draws the menu's chip.

**AND TWO THINGS ARE STILL THE MENU'S, WHICH IS SAID RATHER THAN LEFT TO BE NOTICED.** The ROW HEIGHT is the menu's
(`NSMenuRowHeight`) and the mark's CORNER RADIUS is the menu's constant, so a list's rows are a menu's height and its
fill has a menu chip's rounding. Both are one number each if a list wants its own, and neither is a thing this slice
pretended to have done.

**AND THE EDITOR IS THE WINDOW'S RATHER THAN THE CONTROL'S.** A window lends ONE editor to whichever field is being
typed into (`-fieldEditor:forObject:`), the cell is told to begin a session with it, and the cell is what it reports
back to — because the string belongs to the cell. That door was REFUSED in this tree with the reason "a field editor IS
an `NSText`, and the text system is stage F", written when the tree had no `NSText` at all; `NSText` has existed since
the text slice, so the refusal's ground had expired and the door is a door again. THE TITLE'S MARGIN IS UNCHANGED and
still four points.

## The switch, which is the tier's own control rather than an era door

**Apple has no door for this shape in the era the tree pins.** `NSSwitch` arrived in 10.15; in 10.5 and 10.6 the only
thing shaped like one is `NSSwitchButton`, which is a checkbox with a label. So the sample's pill is drawn by `AGSwitch`
— a track and a knob — and its numbers are the sample's, MEASURED rather than chosen:

| | the sample | as fractions of the height |
|---|---|---|
| the track | **32 x 20**, with both ends round (x=756..787, y=603..622) | width is 1.6 x the height; the radius is half of it |
| the knob | a white circle, **12** across, **flush** with the end it sits at | 0.6 of the track's height |
| OFF | `#E7E7E7` — the same kind of grey as this tier's `+controlColor` | |
| ON | `#A49AFF` — the accent | |
| Prelight | `#E5E3FF` — the accent LIGHTENED | |
| Pressed | the OFF track's own grey — a press is acknowledged by the knob moving | |
| Disabled | `#E4E4E4` | |

**AND THE KNOB'S FLUSHNESS IS A MEASUREMENT RATHER THAN A TIDINESS.** The sample's ON pill runs its accent x=801..819
and its white knob x=822..833, so the track ENDS where the knob does: the only air around the knob is the vertical kind
that comes from its being smaller than the track. Putting air at the ends as well would have been a guess with an answer
already sitting in the file.

## The prelight, which is a departure rather than a reading

**The sample states its four control states outright**, in a legend at the bottom right of the widgets panel — four
chips with their names written on them — so this is the one part of the mockup that needs no inference at all:

| state | the sample's fill | measured from |
|---|---|---|
| Normal | `#BDBDBD` (189,189,189) | the "Normal" chip, interior x=847..919 |
| Prelight | `#E5E3FF` (229,227,255) | the "Prelight" chip, interior x=929..1001 |
| Pressed | `#A49AFF` (164,154,255) | the "Pressed" chip — the accent |
| Disabled | `#E4E4E4` (228,228,228) | the "Disabled" chip |

**AND THE SAMPLE'S PRELIGHT IS A LIGHTENED ACCENT — `#E5E3FF` against the `#A49AFF` it lightens.** THIS TIER DOES NOT
PAINT THAT. A control the pointer rests on takes a GREY one step DARKER than its resting face, and the accent is left
to the press. It is a DECISION BY THE DESIGN OWNER rather than something read off the mockup, which is exactly why it
is written here: the sample disagrees with it, and a departure nobody wrote down is one that gets "corrected" back.

**AND THE RULE HAS ONE EXCEPTION, WHICH THE SAMPLE ITSELF IS THE AUTHORITY FOR: A SWITCH.** The sample's own four-state
legend gives a switch's prelight as `#E5E3FF` — the accent LIGHTENED, which is the very thing this section departs from
— and the departure does not reach it: a switch's track IS the accent when it is ON, so "a step down the grey scale" has
nowhere to go, there being no grey to step down FROM. So `+cg_accentPrelightColor` is back, the switch is its one user,
and the rule above is left exactly as it was for the controls it was about. AN OFF SWITCH IS THE OTHER HALF OF THE SAME
FACT: its track IS a grey, so where the sample draws no prelight at all this tier's rule applies and it steps down like
every other grey face — and the two are tellable apart in the ink itself, one being a grey and the other a tint.

Three things follow, and they are the whole argument for it:

- **The accent means ONE thing.** In this tier it is the press — a button held down, a pop-up whose menu is up — and
  it is already the selection's colour everywhere else (the bar's highlighted item, a list selection). A prelight that
  was also an accent made the same hue say "being pointed at" as well, at a lightness of its own.
- **The fill and the ink stop sharing one channel.** With a grey prelight the ink can stay `+controlTextColor` under the
  pointer and turn white only under the press, so *where the pointer is* and *what is being used* become two
  statements instead of one colour trying to carry both. MEASURED: the darkest ink inside a button's face is 0.00
  idle, 0.00 prelit and 0.75 pressed.
- **A control stays in one family of colour.** The greys read down a single line — window 0.93, face 0.90, prelight
  0.88, disabled 0.86 — which is the same relationship the field's fill already departed for.

**AND `+cg_accentPrelightColor` IS RETIRED WITH THE RULE**, rather than kept as a door with no readers. It WAS the
lightened accent; its only callers were the prelight branches this change replaces. `NSColor.m` deletes one grey that
two names pointed at; a colour with no readers is the same drift read the other way.

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
