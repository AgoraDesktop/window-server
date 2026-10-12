# The window server's tests.
#
# THE SERVER IS TESTED AGAINST ITS OWN CLIENT, not against libagora-app's: a component consumes another only as
# that component's built output, and the window server does not depend on the AppKit — the AppKit depends on it.
# `client.c` is therefore the smallest thing that is a real Wayland client, and what the test asserts is where
# its pixels landed in the server's own screenshot.

find_program(PYTHON3 python3 REQUIRED)

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
add_executable(window-server-test-client client.c "${_generated}/agora-shell-protocol.c")
add_dependencies(window-server-test-client agora-shell-bindings)
target_include_directories(window-server-test-client PRIVATE "${_generated}")
target_link_libraries(window-server-test-client PRIVATE PkgConfig::WAYLAND_CLIENT)

add_test(NAME window-server-headless
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-headless.sh"
	  "$<TARGET_FILE:window-server>" "$<TARGET_FILE:window-server-test-client>"
	  "${PYTHON3}" "${CMAKE_CURRENT_SOURCE_DIR}/check.py")
