# window-server

**Agora's window server** — a Wayland compositor, and the program every Agora application's windows are shown
by.

Agora is an installable desktop environment for Linux and BSD; see the project README in the parent directory
(`../README.md`). This is the bottom of the desktop: above it are the AppKit, and the applications.

## Status

**The first version works: it accepts clients, gives them surfaces, composites what they draw, and screenshots
it.** Two tests hold it up, and the second is the one that matters.

### 1. The AppKit renders into it

`libagora-app`'s `NSWindow` — a real window, a real `NSView` overriding `-drawRect:`, pumped through
`NSApplication` — pointed at **this** compositor instead of at mutter:

```
$ ./build/window-server --frames 90 --screenshot /tmp/agora-ws.ppm --socket awsrun1 &
$ WAYLAND_DISPLAY=awsrun1 libagora-app/build/tests/appkit_window --seconds 1
appkit_window: 0 event(s), drew 90 frame(s); frame now 640x422
window-server: 90 frame(s) composited
window-server: wrote /tmp/agora-ws.ppm

$ python3 ../libagora-app/tests/inspect.py /tmp/agora-ws.ppm
red marker   : 2304 px at (40,62)..(87,109)      (48x48, the window's top-left)
green marker : 2304 px at (632,414)..(679,461)   (48x48, its bottom-right)
window on screen: 640x400 at (40,62)
white bar    : x 614..677
  [ok] red marker is the top-left corner        [ok] green marker is the bottom-right corner
  [ok] both markers are square                  [ok] gradient R rises left to right (36 -> 198)
  [ok] gradient G rises top to bottom (43->150) [ok] a white bar is present
```

That is the whole stack: a view's `-drawRect:`, through `NSWindow`, through the seam, into **Agora's own
compositor**, out as a screenshot — and the area around the window is the compositor's own background
`(48,51,56)`, not a client's.

### 2. It is tested without the AppKit

`ctest` — the server, a client that is nothing but the protocol, and a check that reads the server's own
screenshot:

```
  [ok] the output is 1280x720
  [ok] the client's colour is at the cascade (40,40)
  [ok] the window is 320x200
  [ok] nothing of the client is outside it
  [ok] and the rest is the compositor's own background
check: 5 checks, 0 failed -- all checks passed
```

The server does **not** depend on `libagora-app` — the dependency runs the other way — so its own test uses
`tests/client.c`, the smallest thing that is a real Wayland client.

## What this version deliberately does not do

Each of these is recorded at the code that would have to change (`src/server.h`), because an absent feature
that is written down is a decision and one that is not is a surprise.

- **No real output.** The output is HEADLESS: a framebuffer in this process, described to clients through
  `wl_output`. A DRM/KMS backend and a nested-in-X11/Wayland backend are next; the compositing code does not
  care which, because it is only ever given a width, a height and a place to put pixels.
- **No input devices.** `libinput` is the dependency that would bring them and it is not taken yet. The seat
  advertises a keyboard and sends the **keymap** — so a client's key translation is exercised against a
  compositor-supplied map — but generates no key or pointer events. The pointer is not advertised at all: a
  capability is a promise.
- **No drawing of its own.** The menu bar, the dock and the window frames are the window server's to draw in
  a Mac-like desktop, and this owns none of them yet — which is the honest reason there is no drawing library
  in the build. The compositing is a row copy.
- **No policy.** Every toplevel is placed by a cascade and sized by its client. Placement, stacking, the menu
  bar's reserve and the dock's are POLICY, and policy is what this version has none of.

## Build and run

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=clang
cmake --build build
cd build && ctest

# run it, and point a client at it
./build/window-server --socket agora &
WAYLAND_DISPLAY=agora some-wayland-client
```

Two system libraries, neither vendored: `wayland-server` (the protocol, the event loop and the shared-memory
buffer plumbing, which libwayland owns through `wl_display_init_shm`) and `xkbcommon` (the keymap a seat
sends). The xdg-shell bindings are **generated** at build time from the system's `wayland-protocols`, so a
checked-in copy cannot silently age against the protocol on the machine.

## Licence

MIT — see [`LICENSE`](LICENSE).
