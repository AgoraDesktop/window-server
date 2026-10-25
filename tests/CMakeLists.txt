# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# The window server's tests.
#
# THE SERVER IS TESTED AGAINST ITS OWN CLIENT, not against libagora-app's: a component consumes another only as
# that component's built output, and the window server does not depend on the AppKit — the AppKit depends on it.
# `client.c` is therefore the smallest thing that is a real Wayland client, and what the test asserts is where
# its pixels landed in the server's own screenshot.

find_program(PYTHON3 python3 REQUIRED)
find_package(JPEG REQUIRED)

# xdg-shell, the CLIENT side, which the test client needs and the server does not.
add_custom_command(
  OUTPUT "${_generated}/xdg-shell-client-protocol.h"
  COMMAND ${WAYLAND_SCANNER} client-header "${XDG_SHELL_XML}" "${_generated}/xdg-shell-client-protocol.h"
  DEPENDS "${XDG_SHELL_XML}" VERBATIM)
add_custom_target(xdg-shell-client-bindings
  DEPENDS "${_generated}/xdg-shell-client-protocol.h")

# THE INTERFACE DEFINITIONS ARE SHARED: the private code defines the wl_interface structs and is the same for
# either end of the protocol, so the one the server already generates is compiled in here too rather than
# generated twice.
add_executable(window-server-test-client client.c
  "${_generated}/agora-shell-protocol.c"
  "${_generated}/linux-dmabuf-v1-protocol.c")
add_dependencies(window-server-test-client agora-shell-bindings linux-dmabuf-bindings)
target_include_directories(window-server-test-client PRIVATE "${_generated}")
target_link_libraries(window-server-test-client PRIVATE PkgConfig::WAYLAND_CLIENT JPEG::JPEG)

# **AND THE CLIENT'S HALF OF A GPU BUFFER, WHEN THE MACHINE HAS ONE.** `--dmabuf` allocates on a render node through
# gbm and needs libdrm's headers for the format and modifier names. Absent, the flag still exists and says it cannot
# run — a test that silently stopped testing what it named would be worse than one that fails.
pkg_check_modules(GBM IMPORTED_TARGET gbm)
if(GBM_FOUND)
  target_compile_definitions(window-server-test-client PRIVATE AWS_HAVE_GBM)
  target_link_libraries(window-server-test-client PRIVATE PkgConfig::GBM)
  pkg_check_modules(LIBDRM IMPORTED_TARGET libdrm)
  if(LIBDRM_FOUND)
    target_link_libraries(window-server-test-client PRIVATE PkgConfig::LIBDRM)
  endif()
endif()

# **AND THE WORK AREA, WHICH A RESERVATION MOVES WITHOUT MOVING A PIXEL.** `set_reserved` is the door a Dock asks
# through, and nothing in a screenshot can show that the screen's usable part changed — so this probe connects,
# reserves strips, and asserts the number `agora_output.work_area` carries against the server's own arithmetic.
add_executable(window-server-work-area-probe work_area.c "${_generated}/agora-shell-protocol.c")
add_dependencies(window-server-work-area-probe agora-shell-bindings)
target_include_directories(window-server-work-area-probe PRIVATE "${_generated}")
target_link_libraries(window-server-work-area-probe PRIVATE PkgConfig::WAYLAND_CLIENT)

# **AND THE WINDOW LIST, WHICH IS WHAT A DOCK READS AND WHAT IT DOES WITH A ROW.** The probe asks for the snapshot,
# makes two windows of its own, activates one, asks the other to close, and checks that an id nobody answers is
# ignored — the four things a shell component needs from the server and no application ever asks for.
add_executable(window-server-window-list-probe window_list.c "${_generated}/agora-shell-protocol.c")
add_dependencies(window-server-window-list-probe agora-shell-bindings)
target_include_directories(window-server-window-list-probe PRIVATE "${_generated}")
target_link_libraries(window-server-window-list-probe PRIVATE PkgConfig::WAYLAND_CLIENT)

# **AND THE POINTER'S ROUTING, AS A UNIT TEST RATHER THAN A SCREENSHOT.** The headless backend has no pointer, so a
# drag leaving its window cannot be photographed here — but the RULE that decides where a pointer event goes can be
# asked directly, and `../src/seat.c` is compiled in as it ships. The two questions it asks of the rest of the server
# are stubbed in the test: what is under a point, and how to tell a window where the pointer is.
add_executable(window-server-seat-routing seat_routing.c "${CMAKE_CURRENT_SOURCE_DIR}/../src/seat.c")
target_include_directories(window-server-seat-routing PRIVATE "${_generated}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(window-server-seat-routing PRIVATE
  PkgConfig::WAYLAND_SERVER
  PkgConfig::XKBCOMMON)
add_test(NAME window-server-seat-routing COMMAND window-server-seat-routing)

# **AND THE POINTER THE COMPOSITOR DRAWS ITSELF, WHICH IS WHAT A DRM OUTPUT HAS AND NOTHING ELSE DOES.** The arrow's
# raster and its placement are arithmetic about device pixels — a rounded box, a hotspot, a clipped blend — so they are
# asked directly here; a headless session deliberately draws no cursor, which is why no screenshot could make the
# claim. `../src/cursor.c` is compiled in as it ships.
add_executable(window-server-cursor cursor.c "${CMAKE_CURRENT_SOURCE_DIR}/../src/cursor.c")
target_include_directories(window-server-cursor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(window-server-cursor PRIVATE PkgConfig::WAYLAND_SERVER)
add_test(NAME window-server-cursor COMMAND window-server-cursor)

# **AND THE SYSTEM MENU'S IDENTITY AND MARKS, AS A UNIT TEST.** The detection fixtures, the resolver and every
# coverage grid are checked without a display. `osdetect.c`, `systemicon.c`, the generated table and the generated
# vector data are compiled in as they ship. No Wayland library is needed because nothing here asks the server or the
# display.
add_executable(window-server-system-icon
  system_icon.c
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/osdetect.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/systemicon.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/systemicon-data.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/systemicon-table.c")
target_include_directories(window-server-system-icon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_test(NAME window-server-system-icon COMMAND window-server-system-icon)

# **AND 72 POINTS MUST BE AN INCH, WHICH IS ARITHMETIC AND NOT A DISPLAY.** The policy the backends share — the
# panel's measured dpi, with 96 dpi only as the fallback when it reports no usable size — is compiled in as it ships
# and asked directly: no Wayland server, no EDID, and no screenshot can show that a 91.8-dpi screen got 1.28 rather
# than a rounded 1 or 2. `scale.h` is deliberately free of Wayland so this test needs neither the server headers nor a
# display.
add_executable(window-server-scale
  scale.c
  "${CMAKE_CURRENT_SOURCE_DIR}/../src/scale.c")
target_include_directories(window-server-scale PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_test(NAME window-server-scale COMMAND window-server-scale)

# **AND THE OVERRIDE THE UNIT TEST CANNOT SEE: THE COMMAND LINE.** `--dpi 144` must reach the framebuffer as 2x, the
# startup line must name both spellings, and `--dpi` together with `--scale` must be refused rather than letting one
# silently win. It is a headless server with no client and a one-second deadline, so it needs no display.
add_test(NAME window-server-dpi-override
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-dpi-override.sh" "$<TARGET_FILE:window-server>")

# **THE DIAGNOSTIC DOORS ARE TESTED WITHOUT A DISPLAY**, which is the property that makes them useful on a machine
# whose compositor will not start. `--dump-os-identity` and `--list-system-menu-icons` return before the backend is
# created; an unknown override is rejected by the same path.
add_test(NAME window-server-system-menu-diagnostics
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-diagnostics.sh" "$<TARGET_FILE:window-server>")

# **AND THE CHECKED-IN TABLE MUST MATCH THE TSVs.** The C is generated and committed so a build needs no Python;
# this test regenerates to a temporary file and compares, which is what stops a data edit from silently disagreeing
# with the table the server actually links.
add_test(NAME window-server-system-menu-table
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-icon-table-check.sh"
          "${PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/..")

# **AND EVERY MARK, IN THE SERVER'S OWN SCREENSHOT.** The unit test proves what the coverage grid contains; this
# proves the server was actually told the right mark, painted it in the System menu box, and painted nothing else in
# the bar. One client commits a frame so the render loop has a reason to run, then the server is stopped and asked
# for its screenshot.
add_test(NAME window-server-system-menu-icons
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-system-menu-icons.sh"
          "$<TARGET_FILE:window-server>" "$<TARGET_FILE:window-server-test-client>"
          "${PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/check_icon.py")

add_test(NAME window-server-headless
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.sh"
	  "$<TARGET_FILE:window-server>" "$<TARGET_FILE:window-server-test-client>"
	  "${PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/check.py")

# **AND THE WORK AREA AS A CLIENT SEES IT.** The headless suite above photographs windows; this one asks the server
# what is LEFT of the screen after a client reserves a strip — the arithmetic a Dock depends on and the one thing a
# frame cannot show.
add_test(NAME window-server-work-area
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-work-area.sh"
	  "$<TARGET_FILE:window-server>" "$<TARGET_FILE:window-server-work-area-probe>")

# **AND THE DOCK'S DATA SOURCE.** A snapshot of every window, re-sent when the set changes, plus the two requests a
# shell makes ON somebody else's window: activate (raise and focus) and close (ask, do not destroy).
add_test(NAME window-server-window-list
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-window-list.sh"
	  "$<TARGET_FILE:window-server>" "$<TARGET_FILE:window-server-window-list-probe>")

# **AND WHEN A BUFFER IS GIVEN BACK, WHICH IS A PROTOCOL FACT AND NOT A PAINTING.** The client alternates two buffers
# and counts the releases: one per replaced buffer, none for the one on the glass. It is the invariant behind the
# blink a focus change used to show (README.md §2c), and no pixel test in this tree can see it.
add_test(NAME window-server-release
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-release.sh"
	  "$<TARGET_FILE:window-server>" "$<TARGET_FILE:window-server-test-client>")

# **AND THE GPU-BUFFER PATH, AS A PIXEL COMPARISON RATHER THAN A CLAIM.** The same window is photographed twice —
# once in shared memory, once in GPU memory — and every pixel must match. It needs a display and a DRM render node,
# so it SKIPS (77) on a machine that cannot run it rather than failing there.
add_test(NAME window-server-dmabuf
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-dmabuf.sh"
          "$<TARGET_FILE:window-server>" "$<TARGET_FILE:window-server-test-client>")
set_tests_properties(window-server-dmabuf PROPERTIES SKIP_RETURN_CODE 77)

# **AND THE KEYBOARD'S VERSION BOUNDARY, AS A LIVE PROTOCOL TEST.** `wl_keyboard.repeat_info` is a version-4 event
# and the seat is advertised at 5, so a client that binds 1–3 gets a keyboard resource that must never see it. The
# probe is the smallest real client — registry, seat, keyboard — and the runner binds 1, 2, 3, 4 and 5 in turn.
add_executable(window-server-seat-version-probe seat_version.c)
target_link_libraries(window-server-seat-version-probe PRIVATE PkgConfig::WAYLAND_CLIENT)
add_test(NAME window-server-seat-versions
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-seat-versions.sh"
    $<TARGET_FILE:window-server> $<TARGET_FILE:window-server-seat-version-probe>)

# **AND A MAPPED SURFACE'S DESTRUCTOR, DRIVEN FOR REAL.** `surface_destroy_resource` must take a dying surface out of
# the focus candidates BEFORE the seat reassigns the keyboard, or the reassignment chooses the surface being freed.
# The probe maps two windows, destroys the topmost focused one, and asserts the keyboard reaches the window underneath
# — the client-visible half of an ordering whose other half is a use-after-free.
add_executable(window-server-surface-lifetime-probe surface_lifetime.c "${_generated}/agora-shell-protocol.c")
add_dependencies(window-server-surface-lifetime-probe agora-shell-bindings)
target_include_directories(window-server-surface-lifetime-probe PRIVATE "${_generated}")
target_link_libraries(window-server-surface-lifetime-probe PRIVATE PkgConfig::WAYLAND_CLIENT)
add_test(NAME window-server-surface-lifetime
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-surface-lifetime.sh"
    $<TARGET_FILE:window-server> $<TARGET_FILE:window-server-surface-lifetime-probe>)

# **AND THE WHEEL, AS A VERSION SWEEP.** `wl_pointer.axis` is version 1; `axis_source` and a DISCRETE step count arrive
# at 5, and 8 spells the same steps as `axis_value120` instead. Sending both spellings is a double count and sending a
# v8 event to a v4 client is a protocol violation, so each bound version is driven separately and the exact sequence is
# asserted. The seat is compiled in as it ships and the five `wl_pointer_send_*` doors are interposed.
add_executable(window-server-pointer-axis pointer_axis.c "${CMAKE_CURRENT_SOURCE_DIR}/../src/seat.c")
target_include_directories(window-server-pointer-axis PRIVATE "${_generated}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(window-server-pointer-axis PRIVATE PkgConfig::WAYLAND_SERVER PkgConfig::XKBCOMMON)
add_test(NAME window-server-pointer-axis COMMAND window-server-pointer-axis)

# **AND A BUFFER BIGGER THAN THE SURFACE IT DESCRIBES.** `wl_surface.set_buffer_scale` is how a HiDPI client says "this
# buffer is 2x the frame I asked for". Until the compositor kept that number the request was an empty function, so a
# client that honoured a scale would have overflowed its own frame by the factor. The client draws four colours into a
# 2x buffer and the screenshot proves the WHOLE of it was downsampled into the 1x frame: all four quarters present, and
# nothing outside the frame.
add_test(NAME window-server-buffer-scale
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-buffer-scale.sh"
    $<TARGET_FILE:window-server> $<TARGET_FILE:window-server-test-client>
    "${PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/check_scale.py")

# **AND A 2x OUTPUT, WHICH IS THE HALF THAT MAKES A HIDPI SCREEN USABLE WITHOUT TOUCHING THE CLIENTS.** An ordinary
# window is drawn twice as large — the right SIZE, soft pixels — and a client that renders at 2x itself is a 1:1 copy,
# which is the sharp case. The menu bar is furniture and scales with the output rather than shrinking.
add_test(NAME window-server-output-scale
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-output-scale.sh"
    $<TARGET_FILE:window-server> $<TARGET_FILE:window-server-test-client>
    "${PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/check_output_scale.py")

# **AND A FRACTIONAL SCALE, WHICH IS THE CONFIGURATION MOST HIDPI LAPTOPS ACTUALLY SHIP WITH.** 1.5 is not a ratio
# either space divides: 24 logical rows of menu bar are 36 device rows, a logical pixel is one and a half device pixels,
# and a client that renders at 2x (what `wl_output.scale` advertises for a 1.5 output) is composited 2 -> 1.5. The
# assertions are the same ones the integer case makes, with the arithmetic done in device pixels.
add_test(NAME window-server-output-scale-1-5
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-output-scale.sh"
    $<TARGET_FILE:window-server> $<TARGET_FILE:window-server-test-client>
    "${PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/check_output_scale.py" 1.5)

# **AND THE TERMINAL CLAIM'S CONTRACT, WHICH IS MOSTLY ABOUT WHAT IT REFUSES.** Claiming a virtual terminal needs
# `/dev/tty0`, so a case that claimed one would switch the console out from under the suite — the module is compiled in
# as it ships and this pins the parts that hold either way: the "nothing claimed" state is -1 and not zeros,
# `watch` refuses by name, `release` touches nothing, and an unprivileged claim names the privilege and the capability.
# On a machine that CAN claim, it claims, gives it back, and says the unprivileged branch went untested.
add_executable(window-server-session-vt session_vt.c "${CMAKE_CURRENT_SOURCE_DIR}/../src/session-vt.c")
target_include_directories(window-server-session-vt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_test(NAME window-server-session-vt COMMAND window-server-session-vt)

# **AND A SERVER WITH NOTHING TO DRAW MUST EXIT.** `--frames N` counts composited frames, so no client means no exit
# condition at all — MEASURED as a hung shell, and as a claimed terminal that stranded a session. The deadline is about
# the FIRST frame only, so the second half of this test proves it disarms: a client draws, and the server outlives the
# deadline by three times over.
add_test(NAME window-server-idle-timeout
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-idle-timeout.sh"
    $<TARGET_FILE:window-server> $<TARGET_FILE:window-server-test-client>)

# **AND THE SELECTION, BETWEEN TWO PROCESSES.** The compositor owns the claim and never the data: a request turns into a
# pipe whose ends go to the owner and the asker, so what this asserts is that bytes written by one process arrive in
# another — plus the three refusals, which are as much of the contract as the success, and the promise that a selection
# dies with its owner.
add_test(NAME window-server-clipboard
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-clipboard.sh"
    $<TARGET_FILE:window-server> $<TARGET_FILE:window-server-test-client>)
