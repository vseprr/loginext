cmake_minimum_required(VERSION 3.25)
project(loginext LANGUAGES CXX)

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
)

target_include_directories(loginext PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(loginext PRIVATE
    PkgConfig::LIBEVDEV
    PkgConfig::LIBXCB
    PkgConfig::LIBXCB_ICCCM
    Threads::Threads
)

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
