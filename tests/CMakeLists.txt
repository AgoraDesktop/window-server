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
