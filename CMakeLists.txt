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

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
# AND THE CLIENT SIDE TOO, for the TEST CLIENT only: the server speaks the protocol's server end, and the test
# speaks its client end, so the two are different libraries and neither substitutes for the other.
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
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

file(GLOB WINDOW_SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_executable(window-server
  ${WINDOW_SERVER_SOURCES}
  "${_generated}/xdg-shell-protocol.c")

add_dependencies(window-server xdg-shell-bindings)

target_include_directories(window-server PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${_generated}")

target_link_libraries(window-server PRIVATE
  PkgConfig::WAYLAND_SERVER
  PkgConfig::XKBCOMMON)

enable_testing()
add_subdirectory(tests)
