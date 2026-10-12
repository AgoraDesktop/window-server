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

## What is deliberately NOT decided here

- **The protocol's shape** — whether levels are an extension of `xdg-shell`, a layer protocol of our own, or a
  single Agora surface protocol. That is a real design decision and it is not made.
- **Who draws the window frames.** The Mac's answer is the *application* — its own title bar, in its own window,
  which is why this tree's AppKit draws a chrome strip itself (see `NSWindow.m`) — with the server supplying the
  shadow. Whether Agora keeps that is not decided, but the code as it stands assumes it.
- **Whether the status area is one client or many**, and whether it survives the application that put something
  in it. The Mac's answer is that the server hosts it; ours is unwritten.
- **The level numbers.** Apple's names are quoted above; the values behind `kCG…` were not fetched, so nothing
  here depends on them.
