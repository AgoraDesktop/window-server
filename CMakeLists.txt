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
# cannot silently age against the protocol on the machine.
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

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
# AND THE CLIENT SIDE TOO, for the TEST CLIENT only: the server speaks the protocol's server end, and the test
# speaks its client end, so the two are different libraries and neither substitutes for the other.
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

if(AWS_WITH_DRM)
  pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
endif()
if(AWS_WITH_X11)
  pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
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

add_executable(window-server
  ${WINDOW_SERVER_SOURCES}
  "${_generated}/xdg-shell-protocol.c"
  "${_generated}/agora-shell-protocol.c")

add_dependencies(window-server xdg-shell-bindings agora-shell-bindings)

target_include_directories(window-server PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${_generated}")

target_link_libraries(window-server PRIVATE
  PkgConfig::WAYLAND_SERVER
  PkgConfig::XKBCOMMON)

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
  target_link_libraries(window-server PRIVATE PkgConfig::X11)
  message(STATUS "window-server: x11 (nested) backend: on")
else()
  message(STATUS "window-server: x11 (nested) backend: off (AWS_WITH_X11=OFF)")
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
