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

### 3. Input travels the whole way

The seat is driven by the nested backend, and the input was sent STRAIGHT TO THE WINDOW (`XSendEvent`) rather
than by moving the real pointer — a synthetic pointer lands on whatever the window manager stacked on top, and
on a busy desktop that is not this window. An AppKit client on the other end:

```
window: pointer entered at 260,160     the injected motion, in SURFACE coordinates (300,200 less the
window: key 30 down text="a"           window's cascade at 40,40)
window: key 30 up text="a"
```

The keycode that went in was the X keycode 38; the client saw evdev **30** and translated it to `"a"` through
the keymap this compositor sent it. That is the whole chain: X keycode → evdev → compositor → `wl_keyboard` →
the client's own xkbcommon.

**THE FOCUS POLICY IS TWO HALVES AND BOTH ARE NEEDED**: a window takes the keyboard when it is MAPPED, so a
person can type into the first thing that appears, and a CLICK moves it. The first half was missing and it
showed: keys arrived with the focus nil and went nowhere.

### 4. libinput: written, built, and blocked by a permission

`--input libinput` is implemented, links, assigns **seat0** successfully through logind — and then cannot open
a single device. libinput's own log handler (which is production code a compositor wants anyway; without one it
fails in silence) says exactly why:

```
libinput: event0: opening input device '/dev/input/event0' failed (Permission denied).
libinput: event0  - failed to create input device '/dev/input/event0'
... (every device)
window-server: libinput could not be started; falling back
```

**The measured cause, in two parts, and the first is the one that matters:**

```
$ loginctl list-sessions
SESSION  UID USER SEAT  LEADER CLASS   TTY
      1 1000 kyle -     1391   manager -        <- this process's session: NO SEAT
      3 1000 kyle seat0 2191   user    tty2     <- the graphical session

$ getfacl /dev/input/event0
user::rw-          group::rw-          other::---      (owner root, group `input`; no ACL for kyle)
```

logind grants a session's devices to processes **in that session on that seat**, and this one runs in a
`manager` session with no seat — so nothing is granted. And the devices are `root:input` with no user ACL, so
there is no second way in. **`sudo usermod -aG input kyle` (then log out and back in) is what unblocks it**;
the group `rw` is already there, and nothing else has to change.

Notice what is NOT wrong: udev finds the devices, libinput builds them, and the seat assignment succeeds. The
failure is one `open()` and it is the operating system's answer rather than the compositor's.

### 5. DRM/KMS: written, and unrunnable on a machine with no KMS

`--backend drm` is the shipping backend: it opens a card, takes DRM master, allocates a **dumb buffer**, maps
it, points a CRTC at it and hands the mapping to the compositing loop — so frames are written straight onto the
screen with no copy, no GBM, no EGL and no page flipping. What that costs is tearing, which is the honest first
version; `drmModePageFlip` with two buffers is the next refinement and the backend interface already has the
shape for it (`present` exists and is simply unused).

**Two things it must do that a windowed backend never does**, and both are safety rather than features:

- **It takes DRM master**, the kernel's one-claimant lock on the device. An X server or another compositor
  holding it makes this FAIL and say so — which is what makes the backend safe to start by accident: a
  compositor cannot quietly take a display away from the session using it.
- **It puts the previous mode back on the way out**, so leaving is not a screen left in a state only a reboot
  fixes.

**IT DOUBLE-BUFFERS AND FLIPS AT VBLANK, WHICH IS WHAT PAGES ARE** — one buffer is scanned while the compositor
draws into the other, and the new one is handed over at the next vblank, so the display never shows a frame that
is half drawn. **AND THE FLIP IS ASYNCHRONOUS**: the render loop asks the backend `busy` before it draws, and `busy` answers yes
until the flip event has been read off the DRM descriptor, which is on the event loop. So the wait is real but it
is spent in the event loop, where the rest of the compositor keeps working — not inside `present`, where nothing
could. That also paces the compositor to the display's refresh rather than to how fast it can paint.

**THE ATOMIC REPLACEMENT IS NOT YET CONFIRMED ON A SCREEN.** The legacy path was measured
to be inert on this driver (see below) and has been replaced by `drmModeAtomicCommit`. Everything up to the
commit is verified by the dry run; **the commit itself has not been run**, because its failure mode is the
display of the person running it.

**AND THE CLAIM BELOW WAS MADE ONCE WITHOUT BEING CHECKED, AND WAS WRONG** — the numbers are what the
COMPOSITOR did, not evidence that light came out of a monitor, which is a thing only a person looking at the
screen can say:

```
$ ./build/window-server --backend drm --frames 120 &
$ WAYLAND_DISPLAY=… libagora-app/build/tests/appkit_window --seconds 4
window-server: driving /dev/dri/card0 at 1920x1080 (dumb buffer scanout)
window-server: 120 frame(s) composited
appkit_window: drew 120 frame(s); frame now 640x422
server exit=0
$ xdpyinfo | grep dimensions   ->  3840x1080      (the desktop is back on both monitors)
```

`AWS_DRM_DRY_RUN=1` RUNS EVERY STEP UP TO THE MODESET AND STOPS, which is how the setup path above was proven
before anything was allowed to touch a screen:

```
  connector  77 (DVI-D), 11 mode(s)
  mode       1920x1080 (PREFERRED)
  crtc       42
  buffer     1920x1080, pitch 7680, 8294400 bytes, mapped at 0x…
  framebuffer 93 (XRGB8888)
  Master was taken and is given back; nothing was set on the CRTC.
```

**THE LEGACY PATH IS ACCEPTED AND DOES NOT DISPLAY, WHICH IS WHY THE BACKEND IS ATOMIC NOW.** `drmModeSetCrtc`
returns 0 and `drmModeGetCrtc` then reports this framebuffer on this CRTC — and no monitor changes. The
driver's own state says why:

```
connector 77: CONNECTED, encoder_id=0      encoder 76: crtc_id=0
connector 79: CONNECTED, encoder_id=0      encoder 78: crtc_id=0
CRTCs: 42 52 62 72 — every CRTC reports no framebuffer
```

Nothing is bound to anything in the legacy model, and a desktop is nonetheless on the screen: the live path is
**atomic modesetting**, and the legacy ioctls are only being recorded. A test that filled a dumb buffer with
solid red, held it for eight seconds and called `drmModeDirtyFB` produced NO CHANGE ON ANY MONITOR, which is
what settled it.

`drmModeAtomicCommit` is what this backend does now: the connector's `CRTC_ID`, the CRTC's `MODE_ID` blob and
`ACTIVE`, and the primary plane's `FB_ID`/`CRTC_ID`/`SRC`/`CRTC` rectangles, committed with `ALLOW_MODESET` —
plus `drmModeDirtyFB` in `present`, because a driver has to be TOLD the memory it is scanning has changed or it
shows one frame and never another. The dry run reports all of it:

```
connector   77 (DVI-D), 11 mode(s)      mode  1920x1080 (PREFERRED)      crtc  42
plane       36 (PRIMARY)                framebuffer 92 (XRGB8888), mode blob 94
```

**AND THE ONE OBSTACLE ON THIS MACHINE IS THE DRIVER, NOT THE CODE**: X holds the GPU through the proprietary
NVIDIA driver's own interface, so a DRM/KMS modeset has nothing to take. The evidence is in X's log —
`(II) NVIDIA(0): NVIDIA GPU GeForce GTX 1060 6GB` and a validated MetaMode for both DFP outputs — which is why
`drmSetMaster` succeeds, why the legacy state shows nothing bound, and why an accepted atomic commit changes no
pixel. **Verifying this backend therefore needs a display nobody else is driving**: a text VT with the display
manager stopped. `Ctrl+C` is then the way out, and it runs the teardown because signals stop the event loop
rather than killing the process.

**RELEASING THE DISPLAY IS PART OF THE MODESET AND HAS THE SAME RISK.** On the way out the CRTC is turned off
and the connector detached — before the framebuffer and the buffer under it are destroyed, because a CRTC still
scanning them out makes that destruction wrong. If X is not the DRM master (and on this machine it is not, which
is why `drmSetMaster` succeeds), it may not re-assert afterwards, and the way back is a VT switch.

**MEASURED, AND A MACHINE WITH NO KMS SAYS SO:**

```
$ ./build/window-server --backend drm
window-server: /dev/dri/card0 has no KMS resources at all. On a machine with the proprietary NVIDIA
                driver this is KMS being OFF IN THE KERNEL: `nvidia_drm.modeset=1` ...
exit=1

$ ls /sys/class/drm/card0-DP-* | wc -l     -> 0     (no connectors, no CRTCs)
```

The card can render and cannot display, which is the proprietary NVIDIA driver's default. There is no `vkms`
virtual-KMS module on this machine to test against either. So the backend is built, links, enumerates, and
**refuses with the reason** — and the paths to making it testable are `nvidia_drm.modeset=1` (a kernel module
parameter plus an initramfs rebuild and a reboot) or installing `vkms`.

### 6. It is tested without the AppKit

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

- **Three backends, and the shipping one cannot be tested on every machine.** `headless` is a framebuffer in
  this process, `x11` is a nested window on an X server, and `drm` drives a real display through Kernel Mode
  Setting. The first two run anywhere; the third needs a KMS device, and see below for what happens when there
  is not one. A backend is a place to put pixels — a width, a height and a buffer — which is what makes a new
  one a file (`src/backend-*.c`) rather than a change to the compositor.
- **Input has two sources and the OUTPUT does not decide which.** `--input libinput` reads the machine's own
  devices through udev and logind; `--input host` takes the events the nested backend's window is given. They
  are mutually exclusive — both feed one seat, and a device read twice is a pointer that moves at double speed
  — and the default is libinput when it can start, the nested window otherwise. See below for the one thing
  that still blocks libinput on this machine.
- **No cursor.** `wl_pointer.set_cursor` is accepted and does nothing: a nested output's pointer is the HOST's,
  and a second cursor drawn into the window is one the person did not ask for.
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
