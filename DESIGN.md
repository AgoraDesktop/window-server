# Design — the architecture this is being built toward

**Nothing here is implemented.** It is recorded because the pieces that *are* built have to be able to reach it,
and because the difference between "a compositor" and "Agora's window server" is a decision about **who owns
which pixels** — a decision that is cheap to write down now and expensive to retrofit.

## The screen has three owners, not one

The Mac's window server is not one program that draws everything. It is a compositor plus a small amount of
system-owned furniture, plus applications — and the menu bar is the piece where the seams would show if it were
done any other way.

**The menu bar is stitched from parts owned by different processes.** The WindowServer owns the bar's *global*
portion — the strip itself, the Apple menu at its left, and the status area at its right. The **active
application** owns its own menus (its name, File, Edit, …) and draws them in **its own window**, which the
WindowServer places inside the bar at the right level. Nothing is copied between them and there is no shared
surface: they are separate windows at ordered levels, which is what makes the result look like one object.

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
tree's `NSWindow.m` already assumes when it paints its own chrome strip. The server's share is the shadow.

**THE STATUS AREA IS MANY CLIENTS, NOT ONE.** `NSStatusItem` holds `NSView *_fView` (`NSStatusItem.h` 26): a
status item's view belongs to the **application that made it**. So the status area is a row of windows owned by
several applications and *placed* by the server — which is the same shape as the menu bar beside it, and the
reason the server must not host the contents itself.

**RESERVED REGIONS, AND APPLE'S DOOR FOR THEM IS `visibleFrame`.** `NSScreen` declares `- (NSRect)frame` and
`- (NSRect)visibleFrame` side by side (`NSScreen.h` 30–31); the second is the first minus the menu bar and the
Dock. **This tree's `NSScreen` cannot answer it** — it returns the frame, with the deviation recorded at the
door — because there is no menu bar to subtract yet. That is the concrete thing this architecture unblocks.

### And where the Mac's answer does not reach

**THE WIRE FORMAT IS OURS TO CHOOSE.** macOS has no public protocol for any of this; it is the private CGS API,
so there is no answer to copy at that layer. The **model** above is Apple's — levels, reserved regions, and a
server that places windows it does not own — and the **encoding** is Agora's, because there is nothing to be
compatible *with*. Saying so is the point: "the Mac's answer" is a rule about the model, and pretending it
extends to a wire format that Apple never published would be inventing one and calling it compatibility.

**THE LEVEL NUMBERS ARE UNCHANGED AND STILL UNUSED.** Apple's *names* are quoted above; the `kCG…` values were
not fetched, and nothing here depends on them — an Agora client will name a level the way an AppKit client does,
and the numbers are the server's business.
