cmake_minimum_required(VERSION 3.25)
project(loginext LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)

add_executable(loginext
    src/main.cpp
    src/config/args.cpp
    src/config/loader.cpp
    src/core/device.cpp
    src/core/emitter.cpp
    src/core/event_loop.cpp
    src/core/pacer.cpp
    src/heuristics/scroll_state.cpp
)

target_include_directories(loginext PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(loginext PRIVATE
    PkgConfig::LIBEVDEV
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
