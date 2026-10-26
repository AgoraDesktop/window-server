# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

#===----------------------------------------------------------------------===#
#
# Agora's build for the window server.
#
# THE WINDOW SERVER IS A WAYLAND COMPOSITOR. It is the program `libagora-app`'s windows are ultimately shown by,
# and this is its first version: clients, surfaces, xdg-shell, shared-memory buffers, and a HEADLESS output with
# a screenshot. See README.md for what it deliberately does not do yet, and src/server.h for the same list at
# the code that would have to change.
#
# TWO SYSTEM LIBRARIES, AND NEITHER IS VENDORED: `wayland-server` (the protocol, the event loop and the
# shared-memory buffer plumbing) and `xkbcommon` (the keymap a seat sends). The xdg-shell BINDINGS ARE
# GENERATED at build time from the system's `wayland-protocols` by `wayland-scanner`, so a checked-in copy
# cannot silently age against the protocol on the machine. The optional backends add their own — libdrm/GBM for
# DRM and libX11/libXext/libXrender for the nested one — and the pointer's THEME is the one dependency that is
# data rather than a library: `assets/cursors/Vimix` is installed and read by `src/cursor-theme.c`.
#
#===----------------------------------------------------------------------===#

cmake_minimum_required(VERSION 3.24)

project(AgoraWindowServer LANGUAGES C)

include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# WHICH BACKENDS ARE BUILT IS A DECISION, NOT A GUESS.
#
# These were AUTO-DETECTED: pkg-config found a library, so the backend was
# compiled in. That made the shipped binary depend on what the BUILD HOST
# happened to have — a package built where libX11 was installed got an X11
# backend, one built where it was not did not, and nothing recorded which.
# Pinning a release does not help while the feature set still floats with the
# machine.
#
# Now each option decides. When an option is ON its dependency is REQUIRED, so a
# requested backend whose library is missing fails the configure instead of
# silently vanishing; when it is OFF the library is not looked for at all, so a
# present-but-unwanted library cannot slip into the binary.
#
# THE DEFAULTS ARE THE SHIPPING INTENT. The DRM/KMS output is the one that ships.
# The X11 backend is a development convenience — run the compositor nested in a
# window — and the top-level README's "no X11-native target" keeps it out of the
# shipped thing. libinput is the real input path; the note at its option records
# the one thing about it that is not settled.
# ---------------------------------------------------------------------------
option(AWS_WITH_DRM      "build the DRM/KMS output backend (the shipping output)" ON)
option(AWS_WITH_X11      "build the nested X11 backend (development only)"        OFF)
option(AWS_WITH_LIBINPUT "build the libinput device-input path"                   ON)
# **AND WHO DRAWS THE SCENE.** The CPU renderer is always built, because every backend can run it. The GL one is
# optional and needs a native drawable: an X11 window from the nested backend, or a GBM surface from the DRM one. So
# the option is ON and the feature follows AWS_WITH_X11 or AWS_WITH_DRM — asking for a GL renderer in a build that
# cannot host one would be a configure that lies about what it made.
option(AWS_WITH_GL       "build the GPU renderer (EGL/GLES2; an X11 window or a DRM/GBM scanout)" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
# AND THE CLIENT SIDE TOO, for the TEST CLIENT only: the server speaks the protocol's server end, and the test
# speaks its client end, so the two are different libraries and neither substitutes for the other.
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
# **AND THE RECTANGLE ARITHMETIC A FRAME'S DAMAGE NEEDS.** A frame may paint several disjoint rectangles (a client
# that damages two strips, a second surface, the pointer's old place), so the union is a `pixman_region32_t` and not a
# bounding box: one box would repaint the gap between two strips, and a compositor that paints what nobody damaged is
# the thing this is for. pixman is the same library the rest of the tree takes from the system.
pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)

if(AWS_WITH_DRM)
  pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
endif()
if(AWS_WITH_X11)
  pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
  # XShm, which is what makes a present a pointer instead of 5.8 MB on the wire.
  pkg_check_modules(XEXT REQUIRED IMPORTED_TARGET xext)
  # **AND RENDER, WHICH IS HOW A THEMED POINTER REACHES A HOST-DRAWN CURSOR.** X11's own cursors carry a 1-bit mask,
  # so a themed arrow's antialiased alpha would be a jagged silhouette; an ARGB32 picture turned into a cursor is the
  # only shape-preserving route. Required with the backend rather than optional: a nested session that quietly showed
  # the host's cursor instead of the desktop's would be the half-feature this dependency exists to avoid.
  pkg_check_modules(XRENDER REQUIRED IMPORTED_TARGET xrender)
endif()
# **AND THE GPU RENDERER'S LIBRARIES.** EGL is the context, GLES2 the drawing, and GBM the surface a DRM backend
# gives EGL instead of a window. All of them are REQUIRED when the option is on AND the build can host the renderer —
# which is now the X11 backend, the DRM backend, or both. A GL build with no way to show the result is not built.
if(AWS_WITH_GL AND (AWS_WITH_X11 OR AWS_WITH_DRM))
  pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
  pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)
  if(AWS_WITH_DRM)
    pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
  endif()
endif()
if(AWS_WITH_LIBINPUT)
  # LIINPUT'S UDEV PATH IS NOT SEPARABLE HERE, AND THE DEVIATION IS RECORDED RATHER THAN HIDDEN.
  # input-libinput.c calls udev_new(), libinput_udev_create_context() and libinput_udev_assign_seat(), so
  # this path needs libinput AND libudev, and the seat assignment goes through logind — both of which the
  # top-level README's "No systemd" rule excludes. THE PROJECT HAS DECIDED TO KEEP IT: real device input
  # outranks the rule here, and the alternative written path does not exist (with this off, the shipping
  # DRM output has NO device input). The shipped compositor therefore links libudev and logind, deliberately.
  pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
  pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)
endif()
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

find_file(XDG_SHELL_XML
  NAMES xdg-shell.xml
  PATHS /usr/share/wayland-protocols/stable/xdg-shell
  NO_DEFAULT_PATH REQUIRED)

set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_generated}")

add_custom_command(
  OUTPUT "${_generated}/xdg-shell-server-protocol.h"
  COMMAND ${WAYLAND_SCANNER} server-header "${XDG_SHELL_XML}" "${_generated}/xdg-shell-server-protocol.h"
  DEPENDS "${XDG_SHELL_XML}"
  VERBATIM)
add_custom_command(
  OUTPUT "${_generated}/xdg-shell-protocol.c"
  COMMAND ${WAYLAND_SCANNER} private-code "${XDG_SHELL_XML}" "${_generated}/xdg-shell-protocol.c"
  DEPENDS "${XDG_SHELL_XML}"
  VERBATIM)

# Both outputs have to exist before anything compiles, and only the .c is a source file.
add_custom_target(xdg-shell-bindings
  DEPENDS
    "${_generated}/xdg-shell-server-protocol.h"
    "${_generated}/xdg-shell-protocol.c")

# AGORA'S OWN PROTOCOL, generated from this repository's contract and NOT vendored: `agora-shell.xml` is the
# source, the bindings are code, and generating them at build time means a contract that does not parse stops
# the build rather than being discovered by a client.
set(AGORA_SHELL_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocol/agora-shell.xml")
add_custom_command(
  OUTPUT "${_generated}/agora-shell-server-protocol.h"
  COMMAND ${WAYLAND_SCANNER} server-header "${AGORA_SHELL_XML}" "${_generated}/agora-shell-server-protocol.h"
  DEPENDS "${AGORA_SHELL_XML}" VERBATIM)
add_custom_command(
  OUTPUT "${_generated}/agora-shell-client-protocol.h"
  COMMAND ${WAYLAND_SCANNER} client-header "${AGORA_SHELL_XML}" "${_generated}/agora-shell-client-protocol.h"
  DEPENDS "${AGORA_SHELL_XML}" VERBATIM)
add_custom_command(
  OUTPUT "${_generated}/agora-shell-protocol.c"
  COMMAND ${WAYLAND_SCANNER} private-code "${AGORA_SHELL_XML}" "${_generated}/agora-shell-protocol.c"
  DEPENDS "${AGORA_SHELL_XML}" VERBATIM)
add_custom_target(agora-shell-bindings
  DEPENDS
    "${_generated}/agora-shell-server-protocol.h"
    "${_generated}/agora-shell-client-protocol.h"
    "${_generated}/agora-shell-protocol.c")

file(GLOB WINDOW_SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

# **AND THE GPU-BUFFER PROTOCOL, WHICH IS A PROTOCOL LIKE ANY OTHER.** `zwp_linux_dmabuf_v1` is stable in
# wayland-protocols, so it is generated from the system's copy rather than vendored — the same rule the two
# protocols above follow, and the reason a client's `wl_buffer` and the server's can never disagree about a format.
find_file(LINUX_DMABUF_XML
  NAMES linux-dmabuf-v1.xml
  PATHS /usr/share/wayland-protocols/stable/linux-dmabuf
  NO_DEFAULT_PATH REQUIRED)
add_custom_command(
  OUTPUT "${_generated}/linux-dmabuf-v1-server-protocol.h"
  COMMAND ${WAYLAND_SCANNER} server-header "${LINUX_DMABUF_XML}" "${_generated}/linux-dmabuf-v1-server-protocol.h"
  DEPENDS "${LINUX_DMABUF_XML}" VERBATIM)
add_custom_command(
  OUTPUT "${_generated}/linux-dmabuf-v1-client-protocol.h"
  COMMAND ${WAYLAND_SCANNER} client-header "${LINUX_DMABUF_XML}" "${_generated}/linux-dmabuf-v1-client-protocol.h"
  DEPENDS "${LINUX_DMABUF_XML}" VERBATIM)
add_custom_command(
  OUTPUT "${_generated}/linux-dmabuf-v1-protocol.c"
  COMMAND ${WAYLAND_SCANNER} private-code "${LINUX_DMABUF_XML}" "${_generated}/linux-dmabuf-v1-protocol.c"
  DEPENDS "${LINUX_DMABUF_XML}" VERBATIM)
add_custom_target(linux-dmabuf-bindings
  DEPENDS
    "${_generated}/linux-dmabuf-v1-server-protocol.h"
    "${_generated}/linux-dmabuf-v1-client-protocol.h"
    "${_generated}/linux-dmabuf-v1-protocol.c")

add_executable(window-server
  ${WINDOW_SERVER_SOURCES}
  "${_generated}/xdg-shell-protocol.c"
  "${_generated}/agora-shell-protocol.c"
  "${_generated}/linux-dmabuf-v1-protocol.c")

add_dependencies(window-server xdg-shell-bindings agora-shell-bindings linux-dmabuf-bindings)

target_include_directories(window-server PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${_generated}")

target_link_libraries(window-server PRIVATE
  PkgConfig::WAYLAND_SERVER
  PkgConfig::XKBCOMMON
  PkgConfig::PIXMAN)

if(AWS_WITH_DRM)
  target_compile_definitions(window-server PRIVATE AWS_HAVE_DRM)
  target_link_libraries(window-server PRIVATE PkgConfig::DRM)
  message(STATUS "window-server: drm (KMS) output backend: on")
else()
  message(STATUS "window-server: drm (KMS) output backend: off (AWS_WITH_DRM=OFF)")
endif()

if(AWS_WITH_LIBINPUT)
  target_compile_definitions(window-server PRIVATE AWS_HAVE_LIBINPUT)
  target_link_libraries(window-server PRIVATE PkgConfig::LIBINPUT PkgConfig::LIBUDEV)
  message(STATUS "window-server: libinput device input: on")
else()
  message(STATUS "window-server: libinput device input: off (AWS_WITH_LIBINPUT=OFF) — no device input")
endif()

if(AWS_WITH_X11)
  target_compile_definitions(window-server PRIVATE AWS_HAVE_X11)
  target_link_libraries(window-server PRIVATE PkgConfig::X11 PkgConfig::XEXT PkgConfig::XRENDER)
  message(STATUS "window-server: x11 (nested) backend: on")
else()
  message(STATUS "window-server: x11 (nested) backend: off (AWS_WITH_X11=OFF)")
endif()

# ---------------------------------------------------------------------------
# THE POINTER'S THEME
#
# The vendored Vimix theme (GPL-3.0, `assets/cursors/Vimix`) is installed as an ordinary freedesktop cursor theme
# and its location is compiled in as the default, the same way `libagora-iconservices` installs and defaults to
# Frost. `AGORA_CURSOR_THEME` overrides it at run time, and a machine with no theme at all still gets a pointer:
# `src/cursor.c` draws one. The path is the INSTALL prefix's, from CMake's own datadir, so a relocatable install
# finds its own theme.
# ---------------------------------------------------------------------------
set(AWS_CURSOR_DEFAULT_THEME "${CMAKE_INSTALL_FULL_DATADIR}/icons/Vimix")
target_compile_definitions(window-server PRIVATE AWS_CURSOR_DEFAULT_THEME="${AWS_CURSOR_DEFAULT_THEME}")
message(STATUS "window-server: pointer theme: ${AWS_CURSOR_DEFAULT_THEME} (AGORA_CURSOR_THEME overrides)")

if(AWS_WITH_GL AND (AWS_WITH_X11 OR AWS_WITH_DRM))
  # `EGL_NO_X11` keeps <EGL/eglplatform.h> from dragging libX11's headers into the renderer: the native handles
  # travel as an opaque pointer and an integer (see `struct aws_backend`), which is the only thing the renderer
  # needs to know about a windowing system — and for GBM the "window" is a GBM surface, not an X window at all.
  target_compile_definitions(window-server PRIVATE AWS_HAVE_GL EGL_NO_X11)
  target_link_libraries(window-server PRIVATE PkgConfig::EGL PkgConfig::GLESV2)
  if(AWS_WITH_DRM)
    target_compile_definitions(window-server PRIVATE AWS_HAVE_GBM)
    target_link_libraries(window-server PRIVATE PkgConfig::GBM)
  endif()
  if(AWS_WITH_DRM AND AWS_WITH_X11)
    message(STATUS "window-server: gl (EGL/GLES2) renderer: on (X11 window and GBM/KMS)")
  elseif(AWS_WITH_DRM)
    message(STATUS "window-server: gl (EGL/GLES2) renderer: on (GBM/KMS)")
  else()
    message(STATUS "window-server: gl (EGL/GLES2) renderer: on (X11 window)")
  endif()
else()
  message(STATUS "window-server: gl renderer: off (AWS_WITH_GL=OFF or no X11/DRM backend)")
endif()

enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Install rules: the program, and the protocol it owns
#
# THE PROTOCOL IS THE PIECE A CLIENT CONSUMES. `protocol/agora-shell.xml` is the
# contract libagora-app generates its client bindings from; libagora-app reads it
# out of this source tree today only because window-server installed nothing.
# Installing it beside the data, and exposing its directory through a CMake
# package, is what lets a consumer stop reaching into a sibling checkout.
# ---------------------------------------------------------------------------

install(TARGETS window-server
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# THE POINTER'S ARTWORK, INSTALLED WHERE AN XCURSOR THEME BELONGS (`share/icons/Vimix/cursors/*`) — the same
# freedesktop theme directory Frost installs into, so the compositor's compiled default finds it AND so does any
# other Xcursor-aware program on the machine (`XCURSOR_THEME=Vimix`).
install(DIRECTORY assets/cursors/Vimix
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)

install(FILES protocol/agora-shell.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/agora/protocol)

include(CMakePackageConfigHelpers)

configure_package_config_file(cmake/AgoraWindowServerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWindowServerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraWindowServer)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWindowServerConfigVersion.cmake
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWindowServerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/AgoraWindowServerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AgoraWindowServer)
