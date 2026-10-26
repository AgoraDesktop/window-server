# Damage: the client names its rectangles, and the compositor paints them

**BOTH HALVES ARE NOW DONE — this document is the record of the work rather than a handoff, and §9 says what is still
open.** A client tells the compositor which pixel rectangles it changed, in buffer pixels, on every commit, and the
compositor's frame is the union of those rectangles in device pixels: one bounding box per frame, clipped in both
renderers, with every non-client change invalidating the whole output by name. MEASURED on this machine, a 320x20 strip
repaint went from 1.617-1.791 ms/frame to 0.208-0.227 ms/frame of server CPU (`headless`, 300 commits, against a build of
the parent commit), and a whole-window commit from 1.520-1.783 to 0.523-0.543 ms/frame because the frame is now the
window and not the screen. `README.md` §2d is the reader-facing account; `src/damage.c` is the arithmetic;
`tests/run-damage.sh` is the test, and it poisons what the client does not damage so that an over-paint is visible.

The steps below were followed in order and each has a "done when"; the notes in place say where the landed code differs
from the plan (the box is passed down the walk so a skipped surface still gets its callback; the GL renderer honours the
box only where its swap preserves; the pointer's old rectangle is still a full frame). Read §6 before touching it — the
traps there are the ones that would otherwise produce a session of "the window flickers and I do not know why".

---

## 1. Where the two halves stand

### The client half — DONE, tested, in `libagora-app` (`6dbda2d`, advanced by `1f5bf63`)

`platform/AGWindowServer.h`:

```c
void AGWindowDamage(AGWindow *window, int x, int y, int width, int height);
int  AGWindowLastDamage(AGWindow *window, int index, int rect[4]);
```

- **Coordinates are BUFFER pixels with a TOP-DOWN origin** — exactly what `wl_surface.damage_buffer` takes, and NOT
  the client's own y-up paint space. `libagora-app` mirrors with `+[NSWindow cg_bufferRowsForDirtyRect:surfaceHeight:]`
  and rounds edges outward; a compositor change must keep that contract (or change both sides together).
- Clamped to the buffer inside the seam, budget of 16 rectangles per frame; past the budget the last rectangle GROWS
  (over-damage is a slower frame, a dropped rectangle is a stale pixel).
- **No damage recorded means "the whole buffer"** — every `-cg_drawFrame` (the whole-frame path) still sends none, and
  so still means everything.
- `AGWindowLastDamage` answers what the last commit actually told the wire; `libagora-app/tests/appkit_partial_damage.m`
  asserts a 20x20 strip repaint reports **one rectangle of 400 pixels out of 66 600**.

The seam's implementation is `platform/AGWindowServer-Wayland.c` (`AGWindowDamage`, the `damage[]`/`last_damage[]`
ledgers on `struct AGWindow`, and the emission at the end of `AGWindowEndFrame`). Nothing there needs to change for
this work — treat it as the client-side contract you are consuming.

### The server half — NOT STARTED

`src/surface.c:213`:

```c
static void surface_damage(struct wl_client *client, struct wl_resource *resource,
                           int32_t x, int32_t y, int32_t width, int32_t height)
{
        (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
        /* DAMAGE IS TRACKED NOWHERE, ON PURPOSE: this compositor recomposites the whole output on every frame, so
         * a damage rectangle could only be used to AVOID work that is already not being done. It is where the
         * optimisation goes when there is a reason for one. */
}
```

That comment is the invitation. The reason is now on the table: a terminal repainting one row of text, a scroller
dragging a knob, an editor blinking a caret.

---

## 2. What the frame path does now (read these five places first)

| | |
|---|---|
| `src/main.c:560-640` | The loop. `wl_event_loop_dispatch` blocks with `-1`, or polls with a deadline when `render_pending` is set and the backend is not busy; a frame is throttled by `AWS_FRAME_NS` (`1/60 s`, `main.c:43`). After dispatch: `if (render_pending && !backend_busy && now >= last + AWS_FRAME_NS) { render_pending = 0; aws_output_render(server); aws_backend_present(server); wl_display_flush_clients(...); }` |
| `src/output.c:184-204` | `aws_output_render` — calls `server->renderer->draw(server)` and **deliberately releases no buffer** (the note there is worth reading before you are tempted to add one). |
| `src/output.c:95-180` | `aws_output_walk` — the composite order: background clears belong to the renderer; `emit(NULL, …)` is the menu bar at `AWS_MENU_BAR_LEVEL`; then each surface in level order; a sheet is drawn with its parent (`aws_surface_drawn_with_parent`); a desktop-only session still gets a bar. |
| `src/renderer-cpu.c:812-826` | `cpu_draw`: **`memset` the whole output to `AWS_BACKGROUND`**, `aws_output_walk(server, cpu_emit)`, then `aws_cursor_draw_cpu`. `cpu_emit` (`:760-811`) draws a window's SHADOW (geometry, not content) and then `composite_surface`. |
| `src/renderer-gl.c:1121-1135` | `gl_draw`: `glViewport(0,0,w,h)`, `glClearColor` + **`glClear(GL_COLOR_BUFFER_BIT)`**, `aws_output_walk(server, gl_emit)`, cursor quad. Same shape, and it must stay the same picture. |

Both renderers are walked by `aws_output_walk`, which is the reason a damage change can be made once at the walk's
level and once per renderer's clip, rather than twice in two hand-written copies of the order.

---

## 3. The design (decided) and the design (open)

### Decided

1. **A surface accumulates its own damage, in buffer pixels, and keeps it until a composite consumes it.** The
   protocol's `damage_buffer` is already in that space; `damage` (surface-local, pre-scale) is converted to it by the
   client's `buffer_scale`. Out-of-bounds rectangles are clamped, and an EMPTY damage list on a commit that attached a
   buffer means "the whole buffer" (Wayland's own rule — a client that attaches and damages nothing is asking for a
   full repaint).
2. **The compositor consumes damage as ONE BOUNDING BOX of output pixels per frame, not a region.** The AppKit caps
   itself at sixteen rectangles and a terminal's are one strip; a region buys corner cases (disjoint strips) at the
   cost of a clip that has to be honoured by every writer. If a profile later says otherwise, a `pixman_region32_t`
   replaces the box without changing anything above it.
3. **Anything that is not a client commit invalidates EVERYTHING** (see §4, step 2). This is the safety valve: the
   feature starts as "a commit can be partial, nothing else can", which is provable, and widens only when a case is
   measured and tested.
4. **Skipping the composite entirely when nothing is damaged comes last**, and only after a frame of "nothing to do"
   is shown not to starve a client's `wl_surface.frame` callback (see §6).

### Open — decide with a measurement, not a taste

- Whether to keep `AWS_FRAME_NS`'s 60 Hz throttle once partial frames are cheap. A partial frame is a fraction of the
  work; the throttle exists to stop a client that commits at input rate from driving the compositor at input rate.
  Keep it until something measures worse with it gone.
- Whether the *shadow* should be recomposited on a content-only damage. It is a function of geometry alone
  (`cpu_emit` draws it from `surface->x/y/frame_width/frame_height`), so it should not be — but a shadow that overlaps
  the window's own damaged rows is *painted over* by the window, so the rows the damage covers must include the
  shadow's ring only if the shadow itself changed (it did not). Assert this with a screenshot, do not reason it out.
- Whether `aws_output_render` gains a `damage` argument or reads `server->damage_box`. Prefer the argument: the walk is
  already a callback and a parameter says what the frame is for.

---

## 4. The plan, in five steps (each one buildable and testable on its own)

### Step 1 — track the damage, change nothing about drawing

- `struct aws_surface` (`src/server.h:74`) gains: `struct aws_box damage;` (or four ints), `int damage_valid;` and
  `int damage_pending;` — semantics: `damage_pending` is "the client damaged this buffer since the last composite".
- `surface_damage()`/`surface_damage_buffer()` union into `damage` (clamped to the current buffer's size); a commit
  that attached a buffer and recorded no damage sets the whole buffer.
- `surface_commit` (`src/surface.c:260`) carries the pending damage onto the surface's committed state — and remember
  the two-buffer rule in the client: damage describes the buffer being committed, so it must be stored with
  `current_buffer` and consumed exactly once.
- `aws_output_render` collects, for every mapped surface with pending damage, the rectangle translated to OUTPUT
  pixels — `surface->x/y` are logical, the output has a scale, and the client's own `buffer_scale` divides its buffer
  — then clears each surface's pending damage. For step 1, still composite the whole output.
- **Done when:** `WINDOW_SERVER_DAMAGE_DIAG=1` prints, per frame, `damage: N surface(s), box WxH at (x,y)` or
  `damage: FULL (reason)`, and `window-server-headless` is still green.

### Step 2 — the full-redraw triggers, named one at a time

Every one of these sets `damage_full = 1` (or an empty box meaning "everything"). This list is the whole safety of the
feature; a missing entry is a stale pixel somewhere a person will eventually photograph.

| Trigger | Where it happens now |
|---|---|
| the pointer moved or changed shape/visibility | `src/seat.c` (pointer motion, `set_cursor`, enter/leave); the OLD cursor rectangle must be repainted, which is why this is a full redraw and not "the union of the two cursor boxes" until step 5 |
| a surface was mapped, unmapped, moved, resized, re-leveled, or had its frame changed | `src/shell.c`, `src/surface.c` (`set_buffer_scale`, attach/detach), `src/server.c:139` |
| the menu bar changed (title, item, status item, the clock) | `src/renderer-cpu.c:732`'s `aws_output_draw_menu_bar` and its callers; also `server.c`'s `aws_schedule_frame` |
| the output changed (size, scale, transform) | `src/output.c`, `src/backend*.c` |
| a client committed a buffer with NO damage recorded | §3 rule 1 |
| a client attached a buffer of a different size | the surface's own box grew; the old pixels must be cleared |

- **Done when:** the diagnostic can account for every full redraw in a scripted run: a click (pointer move), a window
  drag, a title change, a resize — each prints `FULL (reason)` with the reason as a string, and no full redraw appears
  in a run where the client only committed strips.

### Step 3 — the CPU renderer composites only the box

- `cpu_draw` takes the box: clear only that box's rows (not the whole output), walk with a clip, draw the cursor with
  the same clip.
- The clip is honoured by four writers, and there is no fifth: the background fill, `composite_surface`'s destination
  spans (`src/renderer-cpu.c:463`, which already computes a per-row span and can intersect it), the menu bar's fill,
  and `aws_cursor_draw_cpu`. Shadows are drawn only when the box touches the window's ring (step 2 says a geometry
  change is already full, so this will be a no-op in practice — make it correct anyway).
- A surface *entirely outside* the box is skipped, but **its `wl_surface.frame` callbacks still have to be completed**
  (`aws_surface_frame_done`, `src/surface.c:800`) or the client stalls waiting for a frame that will never come. Do
  this by walking every mapped surface once for callbacks and only the intersecting ones for pixels — or by letting
  the walk visit all and having the emit return early after the callback.
- **Done when:** the headless screenshot tests are byte-identical to the full-redraw builds for a scripted sequence
  (draw, draw a strip, draw another strip), and a partial frame's cost scales with the box.

### Step 4 — the GL renderer gets the same box

- `gl_draw` (`src/renderer-gl.c:1121`): `glEnable(GL_SCISSOR_TEST)` + `glScissor(x, y, w, h)` around the clear, the
  walk and the cursor draw, then disable. Consistent state on both success and early return.
- Remember the GL origin is bottom-left while the box is top-down from `main`/`output`; convert once, in one function,
  and say so in a comment.
- **Done when:** the GL screenshot tests match the CPU ones for the same scripted sequence (the tree already compares
  the two renderers' pictures — see `renderer-gl.c`'s header note).

### Step 5 — do not composite when nothing is damaged

- If the frame's box is empty and `damage_full` is false: skip `aws_output_render` + `aws_backend_present`, but still
  flush, and still complete frame callbacks owed to clients (see §6).
- **Done when:** an idle session composites zero frames, and a client that asks for `wl_surface.frame` while idle gets
  its callback (no stall, no busy loop). Measure the compositor's CPU at rest before and after; it should go DOWN and
  the demo should not blink.

---

## 5. Files to touch

| File | What |
|---|---|
| `src/server.h` | `struct aws_surface`'s damage fields; `damage_box`/`damage_full` on `struct aws_server`; the box type and the helpers' declarations |
| `src/surface.c` | `surface_damage`, `surface_damage_buffer`, `surface_commit`, and the frame-callback path in `aws_surface_frame_done` |
| `src/output.c` | `aws_output_emit_fn` / `aws_output_walk` gain the box (or a `struct aws_damage_view`); `aws_output_render` passes it to `draw` |
| `src/renderer-cpu.c` | `cpu_draw`, `cpu_emit`, `composite_surface`, `aws_output_draw_menu_bar` clip |
| `src/renderer-gl.c` | `gl_draw`, `gl_draw_menu_bar`, `gl_draw_surface`, cursor quad: the scissor |
| `src/cursor.c` | `aws_cursor_draw_cpu` clip |
| `src/seat.c`, `src/shell.c`, `src/server.c` | the full-redraw triggers (§4 step 2), each with a reason string |
| `src/main.c` | the loop: pass the box, skip an empty frame, keep the flush |
| `tests/` | a new `damage.c` client or an extension of `tests/client.c`; a `run-damage.sh` + `check_damage.py` in the shape of `run-buffer-scale.sh`/`check_scale.py` |

---

## 6. The traps (each of these has cost this tree a bug already)

1. **A skipped surface still owes its frame callback.** `wl_surface.frame` is a promise; a client that attaches,
   commits, then waits for `frame` and gets nothing does not draw again. `cpu_emit` is where `aws_surface_frame_done`
   is called (`renderer-cpu.c:809`) — so clipping the walk must not clip the callbacks.
2. **The cursor's OLD position is a rectangle nobody drew.** Moving the pointer means the pixels where it was have to
   be repainted from what is underneath; that is why a pointer move is a full redraw until step 5, and why step 5 must
   remember the previous cursor box.
3. **Shadows are geometry, not content.** They are a rounded ring drawn from the window's frame, outside its own
   pixels; a content-only damage must not repaint them, and a geometry change must.
4. **`frame_width` clips a bigger buffer** (`composite_surface`'s note: a status item's 320x200 buffer in a 64x24 box),
   so a damage rectangle in buffer pixels can be larger than the window; clamp in `composite_surface`'s terms, not the
   buffer's.
5. **Two scales, and they are different numbers.** The client's `buffer_scale` divides its buffer into logical pixels;
   the output's scale multiplies logical into device pixels; the composited rectangle is in the *output's* space. Mixing
   them up is a damage rectangle that looks plausible and misses by a hair at 1.332.
6. **A dmabuf surface has no shm pixels** (`wl_shm_buffer_get` is NULL) and the CPU renderer will not draw it anyway;
   damage bookkeeping must not assume a buffer it can read.
7. **The two-buffer rule on the client side is not yours to fix**: `libagora-app` widens its damage by what the buffer
   it is painting into is missing (`_cg_paintMissing`), and it reports the *painted* pieces plus what it *copied*. So a
   partial commit from it may still name a large rectangle for one frame — measure with `APPKIT_DRAW_DIAG=1` before
   concluding the damage is wrong.
8. **Do not release buffers in `aws_output_render`.** The comment at `output.c:190-204` explains what that cost (a
   half-drawn window on the glass, twice).
9. **`render_pending` is the server's "a frame is due" flag, not a damage flag.** Do not overload it; a missing
   invalidation is invisible (a stale pixel) and a spurious one is only slow.
10. **The frame throttle is 60 Hz.** A test that expects N composites in M milliseconds is expecting a specific
    relationship between them; `--frames N` exists so a test can stop on composites rather than on time.

---

## 7. Tests, and how to run them

Copy the shape of an existing one rather than inventing a harness:

```sh
cd /home/kyle/Development/Agora/window-server
cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure
ctest --test-dir build -R "window-server-headless|window-server-buffer-scale|window-server-output-scale" -V
```

- `tests/run-headless.sh` drives `window-server --backend headless --frames N --screenshot /tmp/x.ppm` plus
  `tests/client.c`, and `tests/check.py`/`check_scale.py` compare pixels. `tests/client.c` already has
  `--width/--height/--frames/--colour/--image` and paints four quadrants so a screenshot can tell which part of a buffer
  arrived — a partial-damage client wants the same idea: **paint one strip, then a second, and prove the rest of the
  window is untouched.**
- The new case should assert both directions: (a) a partial commit paints its strip and leaves the rest of the output
  BYTE-IDENTICAL, (b) a full redraw trigger (a pointer move, a window move) repaints everything the trigger touched.
- The client-side door is already testable without a compositor (`libagora-app`'s `appkit-partial-damage`); the
  server-side equivalent is a client that damages and a screenshot that proves the compositor drew exactly that.

---

## 8. Acceptance criteria

1. `ctest` in `window-server` is green, including the two screenshot comparisons (CPU and, where it runs, GL).
2. The nested session (`demo/agora-demo.sh 1280x720`) shows no stale strip, no blink, no missing shadow or menu-bar row,
   for a minute of scrolling, dragging, typing and resizing.
3. With `WINDOW_SERVER_DAMAGE_DIAG=1`, a client that repaints one strip produces frames whose damage box is that strip
   (plus whatever the pointer did), and the diagnostic's `FULL (reason)` lines are explainable one by one.
4. The compositor's CPU at rest and during a scroll goes DOWN against a build of the parent commit — measure with
   `perf stat -e task-clock` or `/proc/<pid>/stat` over a scripted sequence, and put the numbers in the commit message
   (this tree's habit, and the only way the next reader knows the work paid).
5. `libagora-app`'s 193 tests still pass (the client half must not need changing, except possibly to stop widening its
   damage once the server consumes it — that would be a *second* commit with its own numbers).

---

## 9. Cheat sheet

```sh
A=/home/kyle/Development/Agora
source "$HOME/.agents/skills/agora-desktop/scripts/agora-env.sh"

# build + test the compositor
cmake --build "$A/window-server/build" -j"$(nproc)"
ctest --test-dir "$A/window-server/build" --output-on-failure

# a compositor that stops on its own, and a screenshot of what it drew
"$A/window-server/build/window-server" --backend headless --input none --socket agora-h --frames 10 \
  --screenshot /tmp/agora.ppm
WAYLAND_DISPLAY=agora-h "$A/window-server/build/tests/window-server-test-client" \
  --width 320 --height 200 --colour FF0000 --frames 10

# the real thing, nested on this machine's X display (:1)
"$A/demo/agora-demo.sh" 1280x720

# the client's side of the ledger
APPKIT_DRAW_DIAG=1 "$A/libagora-app/build/tests/appkit_controls_draw"
```

Diagnostics that already exist and that this work should not confuse: `AWS_RENDER_DEBUG`, `AWS_INPUT_DIAG`,
`AWS_CURSOR_DIAG`, `APPKIT_LOOP_DIAG`, `APPKIT_DRAW_DIAG`. Add **one** of your own (`WINDOW_SERVER_DAMAGE_DIAG`) and
make it print the box and the reason — a damage bug is invisible in a screenshot of a static window and obvious in a
line of text.

---

## 10. What the work did not close

- **THE POINTER'S OLD RECTANGLE IS STILL A FULL FRAME.** A motion on an output that draws its own cursor calls
  `aws_damage_full(server, "the pointer moved")`, because the pixels where the pointer WAS are not a rectangle any
  client damaged — §6.2. The seat knows `pointer_x`/`pointer_y` and the cursor's device extent, so a union of the old and
  new boxes plus the client damage is the next step; it needs the previous box remembered across the frame, and a
  screenshot test that moves the pointer over a damaged strip.
- **THE GL RENDERER PAINTS THE BOX ONLY WHERE ITS SWAP PRESERVES IT.** `EGL_BUFFER_PRESERVED` is asked for and READ
  BACK; NVIDIA's EGL refuses it (MEASURED), and a GBM scanout buffer is a fresh allocation, so on those the GL path
  composites the whole output. The CPU renderer — which is what the shipping DRM path uses when the GL context is the
  CPU's — is where the measurement above comes from. Fixing this properly is a preserved back buffer or a second
  surface, not a scissor.
- **THE CLIENT STILL WIDENS ITS DAMAGE BY WHAT ITS OTHER BUFFER IS MISSING** (`_cg_paintMissing`), which is correct for
  a two-buffer client and now costs more than it needs to on a compositor that keeps the screen. Narrowing it is a
  `libagora-app` change with its own numbers — §8's criterion 5.
- **A REGION, WHEN A PROFILE ASKS FOR ONE.** The box is decided in `damage.c` and nothing above it knows how the union is
  kept, so a `pixman_region32_t` there is a local change: the renderers would then take a clip list instead of one box.
