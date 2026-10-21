# Design — the architecture this is being built toward

**Nothing here is implemented.** It is recorded because the pieces that *are* built have to be able to reach it,
and because the difference between "a compositor" and "Agora's window server" is a decision about **who owns
which pixels** — a decision that is cheap to write down now and expensive to retrofit.

## The screen has three owners, not one

The Mac's window server is not one program that draws everything. It is a compositor plus a small amount of
system-owned furniture, plus applications — and the menu bar is the piece where the seams would show if it were
done any other way.

**The menu bar is stitched from parts owned by different processes.** The WindowServer owns the bar's *global*
portion — the strip itself, the SYSTEM MENU at its left, and the status area at its right. The **active
application** owns its own menus (its name, File, Edit, …) and draws them in **its own window**, which the
WindowServer places inside the bar at the right level. Nothing is copied between them and there is no shared
surface: they are separate windows at ordered levels, which is what makes the result look like one object.

### The System menu, and the OS mark

**IT IS THE SYSTEM MENU, AND ITS MARK IS THE RUNNING OS'S — WITH THE HAMBURGER AS THE FALLBACK.** Not the Apple menu
and not the Apple logo: that mark is Apple's, and this project reuses none of it — the same reason the SDK's headers
are fetched from a recipe here instead of being committed. The server detects the OS once at startup, resolves it to
its exact mark (with the FreeBSD mark standing in for DragonFlyBSD for now), and gives anything unrecognised the
hamburger (`☰`, U+2630 TRIGRAM FOR HEAVEN). The three-line character says in that position what a logo says there,
and belongs to nobody: it is original to this project and dedicated to CC0 1.0.

**AND IT IS THE ONE THING IN THE BAR THAT IS THE SERVER'S.** The status area is clients'; each application's own
menu titles are that application's; the System menu is about the SYSTEM rather than about any program — so the
server draws it, the server decides how wide it is, and everything else in the bar is placed to the right of it.

That is also why it is the first thing this compositor DRAWS rather than fills. The mark is vector data — subpaths
in a 16×16 box, filled even-odd — rasterized once into a coverage grid and blended with one ink colour. The
hamburger is still three horizontal bars, but it is now the same kind of path the OS marks will be, which is what
keeps the rasterizer from becoming a special case for every logo. `libagora-text` is what the server will reach for
when it has to draw something whose SHAPE it does not already know.


**Apple's own headers say so, in the window server's vocabulary rather than AppKit's.** From `NSWindow.h` in the
10.6 SDK (`Versions/C/Headers/NSWindow.h`, fetched by the recipe in `libagora-text/PROVENANCE.md`):

```c
#define NSMainMenuWindowLevel		 kCGMainMenuWindowLevel
#define NSStatusWindowLevel		 kCGStatusWindowLevel
// NSDockWindowLevel is deprecated
#define NSDockWindowLevel		 kCGDockWindowLevel
```

Three things follow from six lines:

- there is a **level for the application's menu window** and **a level above it for the status area** — two
  different owners, stacked, which is the stitching;
- those levels are named `kCG…`, i.e. they belong to **Core Graphics** — the window server's own side of the
  API — and AppKit merely spells them again. The ordering is the *server's* business, not a widget toolkit's;
- and the Dock has a level of its own, whose AppKit name is **deprecated** — which is what a level looks like
  when the thing that uses it stops being an AppKit window and becomes **a separate application**, which the
  Dock is.

## The contract

The protocol those rules require is [`protocol/agora-shell.xml`](protocol/agora-shell.xml) — three interfaces:
`agora_shell` (the global, handing out `agora_window` and `agora_output`), `agora_window` (`set_frame`,
`set_status_item`, `set_level`, `raise`, `lower`, `set_title`, `set_app_id`, `set_visible`, `set_reserved`,
`set_activatable`, `destroy`; events `close`, `work_area_changed` and `pointer_global`) and `agora_output` (the `work_area` event,
which is `NSScreen.visibleFrame`, and `menu_bar_area`). The bindings are generated from it on every build, so a
contract that does not parse stops the build rather than being discovered by a client.

**`set_reserved` IS THE DOCK'S ONE REAL REQUEST, AND IT IS NOT A SPECIAL CASE FOR THE DOCK.** The rule below — the
server must not grow a special case for "the dock" when a level and a reserved region will do — is what it is
shaped by: a client says how many pixels along which edge are its own, the work area is the output minus the menu
bar minus every such strip, and the Dock is simply the client that asks. A panel on the left asking the same
question gets the same answer, and `agora_output.work_area` is re-sent to everyone when the sum changes.

**IT IS NOT `xdg-shell` WITH EXTRA REQUESTS.** A surface given a role here is not an `xdg_toplevel`, because the
configure cycle the two would share is exactly the inversion above: this server does not tell an application how
big to be, so there is nothing to acknowledge. A client picks one role protocol or the other; the two never mix
on one surface.

## What that means for Agora

**`xdg-shell` cannot express this, which is why it was called the bootstrap protocol and not the destination.**
An `xdg_toplevel` is a window with a title, a size and a position; there is no way to say *"this surface is the
menu bar of the active application"*, or *"this one is the status area, and it is above that"*, or *"the Dock is
here"*. What is needed is a protocol of Agora's own with at least:

- **levels or layers**, so the menu window, the status area, the Dock and ordinary windows can be ordered
  relative to each other rather than by creation order — which is what `NSMainMenuWindowLevel` and
  `NSStatusWindowLevel` are;
- **reserved regions**, so the server can tell a client its work area excludes the menu bar strip and the Dock's
  — which is what `-visibleFrame` means, and what `NSScreen` in this tree currently cannot answer because there
  is no menu bar to exclude;
- **a way for the server to place a window it does not own**, so an application's menu window can be put in the
  bar at the right level without the application knowing where the bar is.

And the clients of that protocol are not only applications: **the Dock is one of them**, and so is whatever draws
the status area. They are ordinary clients with levels, which is exactly how the Mac does it — and it is why the
window server must not grow a special case for "the dock" when a level and a reserved region will do.

## The rule: the Mac's answer is the answer

**Where the Mac has an answer, Agora takes it.** Not because the Mac is right — because a compatible stack is
the point, and an answer that was invented here is an answer no existing code, no existing documentation and no
existing habit will agree with. This is the tie-breaker for every question in this document, and it is written
down so the questions below are answered rather than re-argued.

### The four questions, answered from Apple's headers

**LEVELS, NOT LAYERS.** `NSWindow` declares `- (void)setLevel:(NSInteger)newLevel` and `- (NSInteger)level`
(`NSWindow.h` 453–454). The Mac's model is **a per-window integer level**, in the *window server's* vocabulary
(`kCG…`), not a fixed enumeration of layers with a toolkit's names on them — so an application can put its menu
window at the menu level and its panel above ordinary windows without the server having anticipated either.

**THE APPLICATION DRAWS ITS OWN WINDOW FRAME.** `NSWindow` declares
`+ (NSButton *)standardWindowButton:(NSWindowButton)b forStyleMask:` and `- (NSButton *)standardWindowButton:`
(`NSWindow.h` 574–575): the close, miniaturize and zoom buttons are **`NSButton`s, in the application's own
process**. They are the application's to draw, which is what "client-side chrome" means, and it is what this
tree's `NSWindow.m` already assumes when it paints its own chrome strip. The server's share is the shadow **and
the window's SHAPE**: a window travels the wire opaque (`XRGB8888`, no alpha — which is what keeps text
sub-pixel-smoothed in the client), so a client that rounded its own corners could only PAINT a corner and the square
shoulder would still show around the arc. The compositor therefore clips every window to a rounded rectangle of the
same radius its shadow is drawn with, and leaves the surfaces that are flush with the output — a maximised window,
the menu bar, anything inside the bar's rows — square, because a notch out of the desktop's own edge is not a
corner. Both numbers are `AWS_SHADOW_RADIUS`, six — and the client's `fn_BAR_RADIUS` is the same six, because the
protocol does not carry a radius and the two components each state it.

**THE STATUS AREA IS MANY CLIENTS, NOT ONE — AND THE SERVER IS NONE OF THEM.** `NSStatusItem` holds
`NSView *_fView` (`NSStatusItem.h` 26), and `NSStatusBar` declares `+systemStatusBar`,
`-statusItemWithLength:` and `-thickness`: a status item has a **view, a title, an image, a menu, a target and an
action**, and all of them belong to whoever made the item.

**AND THE SYSTEM'S OWN EXTRAS ARE A SEPARATE PROCESS RATHER THAN THE SERVER, WHICH IS THE POINT WORTH WRITING
DOWN.** "Menu bar extra" is the older name for the things in that area, and the *system's* ones — Wi-Fi, battery,
the clock, sound — were bundles drawn by **SystemUIServer**: a process of its own, the same shape as the Dock.
So the area has two kinds of owner and neither is the window server:

    a system extra (Wi-Fi, the clock)   drawn by SystemUIServer — a separate process
    an application's menu bar item      drawn by the application  (NSStatusItem._fView)

**AND AN ITEM'S POSITION IS THE SERVER'S, WHICH IS THE ONE PLACE THE FRAME RULE DOES NOT REACH.** `NSStatusItem`
declares `-length`, `-title`, `-image`, `-menu`, `-target`, `-action`, `-highlightMode` and `-view` — AND NO
POSITION OF ANY KIND. That is what an item an application placed for itself would need and could not have: an
application does not know what else is in the status bar, so it cannot know where its own item goes. The server
packs them, and the application says only HOW LONG ITS ITEM IS. So `agora_shell` will need a way for a surface to
say "the server places this one" — a KIND of window, not a frame — and `set_frame` will not be called for it.

**A SERVER THAT HOSTED THE CONTENTS WOULD BE MAKING THE MISTAKE THE DOCK RULE EXISTS TO PREVENT**, one area over:
it owns the STRIP and places what is in it, and the moment it also draws an item it has a special case for a kind
of client. The two doors agree on the reserve as well — the server's own reserved height and
`[[NSStatusBar systemStatusBar] thickness]` are the same number — which is a cross-check worth having before
anything is built on it.

**RESERVED REGIONS, AND APPLE'S DOOR FOR THEM IS `visibleFrame`.** `NSScreen` declares `- (NSRect)frame` and
`- (NSRect)visibleFrame` side by side (`NSScreen.h` 30–31); the second is the first minus the menu bar and the
Dock. **This tree's `NSScreen` cannot answer it** — it returns the frame, with the deviation recorded at the
door — because there is no menu bar to subtract yet. That is the concrete thing this architecture unblocks.

**POSITION AND SIZE ARE THE APPLICATION'S, AND THE SERVER HONOURS THEM.** `NSWindow` declares
`- (void)setFrame:(NSRect)frameRect display:` (294), `- (void)setFrameOrigin:` (296) and
`- (void)setFrameTopLeftPoint:` (297) — the application states a frame, position included, and reads it back
with `- (NSRect)frame` (299). There is no door by which the server moves a window, and that is not an omission:
**the only thing that adjusts a frame is `- (NSRect)constrainFrameRect:(NSRect)toScreen:` (293), an `NSWindow`
method** — the application's own AppKit, against a screen — and **both resize decisions are DELEGATE methods**,
`- (NSSize)windowWillResize:toSize:` (666) and `- (NSRect)windowWillUseStandardFrame:defaultFrame:` (667). Even
Zoom is the window's, and the frame it zooms to comes from the application's delegate.

**THIS IS THE OPPOSITE OF WHAT WAYLAND DOES, AND IT IS THE SECOND THING `xdg-shell` CANNOT SAY — LARGER THAN
THE FIRST.** An `xdg_toplevel` is **configured by the compositor**, which sends a size the client is expected to
take; and there is **no request at all** by which a client asks to be put somewhere, because placing windows is
the compositor's job in that model. So the two are inverted on size and silent on position, and a client that
must be honoured cannot be written against it.

**WHAT THAT COSTS IN THE CODE THAT EXISTS**, each one a thing to change rather than a thing to interpret:

- **the compositor's CASCADE IS A POLICY THE MAC DOES NOT HAVE.** `surface_commit` places a new toplevel at an
  offset it chose (`40 + n * 40`). Under this rule the client says where it goes and the compositor writes it
  down; a default is legitimate only for a client that asked for nothing.
- **`-setFrame:` MUST REACH THE SERVER.** `NSWindow.m` currently updates the Cocoa frame and says in its header
  that "a Wayland client does not place or move its toplevel". Under this rule it does, and the door is missing:
  the seam needs a "here is my frame" call, honoured by whichever backend is in use.
- **AND `-constrainFrameRect:toScreen:` MEETS THE PREVIOUS SECTION HERE**, because what it constrains against is
  `[NSScreen visibleFrame]` — the frame minus the menu bar and the Dock. The reserved regions are what make the
  application's own constraint answerable, which is why these two rules are one feature and not two.

**ORDERING FOLLOWS THE SAME RULE**, by the same argument rather than by a separate one: `-orderFront:` raises
the application's own window on the Mac, so the application asks and the server honours. This does not settle
who gets *focus* — on the Mac that is the user's click and the application's `-makeKeyAndOrderFront:` together —
and that is left where it is until it is needed.

### And where the Mac's answer does not reach

**THE WIRE FORMAT IS OURS TO CHOOSE.** macOS has no public protocol for any of this; it is the private CGS API,
so there is no answer to copy at that layer. The **model** above is Apple's — levels, reserved regions, and a
server that places windows it does not own — and the **encoding** is Agora's, because there is nothing to be
compatible *with*. Saying so is the point: "the Mac's answer" is a rule about the model, and pretending it
extends to a wire format that Apple never published would be inventing one and calling it compatibility.

**THE LEVEL NUMBERS ARE UNCHANGED AND STILL UNUSED.** Apple's *names* are quoted above; the `kCG…` values were
not fetched, and nothing here depends on them — an Agora client will name a level the way an AppKit client does,
and the numbers are the server's business.

## The menu bar has a LEVEL now

The strip used to be painted **before** every window, and the source said why that was a placeholder: *"until the
strip has a LEVEL of its own such a window covers it."* The desktop is a full-screen window at a very negative
level, so the strip now has one — **`AWS_MENU_BAR_LEVEL`** (24, Apple's `kCGMainMenuWindowLevel`) — and is drawn in
the level walk just before the first surface at or above it. The **desktop** and ordinary windows are painted
first and the bar covers them; the application's own **menu window** and the **status items**, being at or above
that level, are painted after and sit **in** the bar. A screen with no window at or above the bar still gets one.
