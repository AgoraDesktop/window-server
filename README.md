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

### 2. And it runs nested, in a window on a desktop

`--backend x11` puts the compositor's screen in an X window — the same thing weston's `--backend=x11` and
mutter's `--nested` are. An independent capture of the desktop, with the AppKit client running inside it:

```
$ ./build/window-server --backend x11 --frames 200 --screenshot /tmp/ws-x11.ppm

$ python3 ../libagora-app/tests/inspect.py /tmp/ws-x11.ppm      # the compositor's own output
  [ok] red marker is the top-left corner   [ok] green marker is the bottom-right corner
  [ok] both markers are square             [ok] gradient R rises left to right (36 -> 215)
  [ok] gradient G rises top to bottom      [ok] a white bar is present

$ python3 ... # on an X screenshot taken at the same time
  red marker on the desktop  : 2304 px at (360,277)..(407,324)
  green marker on the desktop: 2304 px at (952,629)..(999,676)
  -> a 640x400 window at (360,277), red top-left, green bottom-right
  the nested window's own corner (320,237) is (48,51,56) — the window server's background
```

**IT GRABS NOTHING, AND THAT IS NOT AN ACCIDENT OF THE CODE YET.** This server has no input devices (no
`libinput`, see below), so the backend presents pixels and takes no input: no `XGrabPointer`, no
`XGrabKeyboard`, no pointer fence. That matters because a nested compositor *with* input can leave a desktop
with a dead mouse — which is a real failure this project has already met once, from a nested `gnome-shell`.
A compositor that has nothing to route has nothing to grab, and that stays true until input arrives on purpose.

**An X11 backend is not an X11 target.** Agora's display target is Wayland on real hardware, with Xwayland for
X *clients*; this is a development backend, which is why it is **optional** — the core links no `libX11` at all
unless it is built in (`-DWITH_X11` is not needed: it is taken if `pkg-config` finds it).

### 3. It is tested without the AppKit

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

- **No real output.** There are two backends and neither is the shipping one: `headless` is a framebuffer in
  this process, and `x11` is a nested window on an X server. A **DRM/KMS** backend is the real one and is not
  written. A backend is a place to put pixels — a width, a height and a buffer — which is what makes a new one
  a file (`src/backend-*.c`) rather than a change to the compositor.
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
