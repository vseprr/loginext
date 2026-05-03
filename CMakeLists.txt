cmake_minimum_required(VERSION 3.25)
# C is enabled because wayland-scanner produces plain ANSI C — the generated
# code lives next to the C++ tree but compiles through the C frontend.
project(loginext LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBEVDEV   REQUIRED IMPORTED_TARGET libevdev)
# libxcb + xcb-icccm power the X11 active-window listener (Phase 2.5).
# Both are tiny protocol libraries — fit the same "thin syscall wrapper"
# slot as libevdev. Required because Phase 2.5 depends on per-app scope.
pkg_check_modules(LIBXCB     REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(LIBXCB_ICCCM REQUIRED IMPORTED_TARGET xcb-icccm)
# wayland-client + the KDE plasma-window-management protocol back the KWin /
# Plasma Wayland active-window listener. The protocol XML is vendored in
# `protocols/` (KDE-licensed, LGPL-2.1) so we don't depend on the
# `plasma-wayland-protocols` package at build time. wayland-scanner itself
# ships in the `wayland` package alongside the runtime client lib.
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_SCANNER_PC REQUIRED wayland-scanner)
pkg_get_variable(WAYLAND_SCANNER_BIN wayland-scanner wayland_scanner)
# libsystemd / sd-bus drives the KWin D-Bus bridge: KDE Plasma 6 stopped
# advertising org_kde_plasma_window_management to regular wayland clients,
# so we publish a tiny KWin script that calls into us over the user session
# bus. sd-bus integrates with our select() loop via sd_bus_get_fd() — no
# extra event loop, no extra thread, single relaxed atomic to the hot path.
pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

# ── Generated wayland protocol bindings ──────────────────────────────
# The XML drives both a header (function-pointer listener structs, opaque
# proxy types) and a private code module (marshalling tables). The header
# is included from C++ (extern "C"); the code module is C, which is why we
# enable both languages in `project()` above.
set(PROTO_XML        ${CMAKE_SOURCE_DIR}/protocols/plasma-window-management.xml)
set(PROTO_GEN_DIR    ${CMAKE_BINARY_DIR}/protocols)
set(PROTO_GEN_HEADER ${PROTO_GEN_DIR}/plasma-window-management-client-protocol.h)
set(PROTO_GEN_CODE   ${PROTO_GEN_DIR}/plasma-window-management-protocol.c)

file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

add_custom_command(
    OUTPUT  ${PROTO_GEN_HEADER}
    COMMAND ${WAYLAND_SCANNER_BIN} client-header ${PROTO_XML} ${PROTO_GEN_HEADER}
    DEPENDS ${PROTO_XML}
    COMMENT "wayland-scanner client-header (plasma-window-management)"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${PROTO_GEN_CODE}
    COMMAND ${WAYLAND_SCANNER_BIN} private-code ${PROTO_XML} ${PROTO_GEN_CODE}
    DEPENDS ${PROTO_XML}
    COMMENT "wayland-scanner private-code (plasma-window-management)"
    VERBATIM
)
# A phony target groups both generated files so anyone who needs them just
# depends on this. Keeps the generation rules out of the executable's source
# list — listener.cpp pulls in the header transitively, but CMake won't see
# that include without help.
add_custom_target(plasma_protocol_gen DEPENDS ${PROTO_GEN_HEADER} ${PROTO_GEN_CODE})

add_executable(loginext
    src/main.cpp
    src/config/args.cpp
    src/config/loader.cpp
    src/core/device.cpp
    src/core/emitter.cpp
    src/core/event_loop.cpp
    src/core/pacer.cpp
    src/heuristics/scroll_state.cpp
    src/ipc/server.cpp
    src/ipc/dispatch.cpp
    src/scope/listener.cpp
    src/scope/rules.cpp
    src/scope/rules_loader.cpp
    src/util/log.cpp
    ${PROTO_GEN_CODE}
)

# The generated header lacks our -Werror=pedantic discipline (it has
# trailing commas, ISO-C struct-with-no-name warnings depending on version,
# etc). It's machine-generated and known-good — exempt it explicitly.
set_source_files_properties(${PROTO_GEN_CODE} PROPERTIES
    LANGUAGE       C
    COMPILE_FLAGS  "-w"
)

target_include_directories(loginext PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PROTO_GEN_DIR}
)

target_link_libraries(loginext PRIVATE
    PkgConfig::LIBEVDEV
    PkgConfig::LIBXCB
    PkgConfig::LIBXCB_ICCCM
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::LIBSYSTEMD
    Threads::Threads
)

add_dependencies(loginext plasma_protocol_gen)

target_compile_options(loginext PRIVATE
    -O2
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

# --- Optional performance flags (suitable for single-machine daemon) ---

option(LOGINEXT_LTO "Enable link-time optimization" ON)
if(LOGINEXT_LTO)
    set_property(TARGET loginext PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

option(LOGINEXT_NATIVE "Use -march=native (non-portable binary)" ON)
if(LOGINEXT_NATIVE)
    target_compile_options(loginext PRIVATE -march=native)
endif()
