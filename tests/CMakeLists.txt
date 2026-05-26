# Unit test target for LogiNext modules with pure logic that can be
# exercised without a real /dev/input device, uinput, sd-bus, or X11.
#
# Strategy: each test executable links against the relevant production
# source files directly. We don't carve out a shared static lib yet —
# at this scale (two modules under test) the direct-link approach keeps
# CMake noise low. Revisit when there are 4+ test executables and
# meaningful build-time overhead from recompiling the same TUs.

# ─── heuristics tests ────────────────────────────────────────────────
add_executable(test_heuristics
    heuristics/test_scroll_state.cpp
    ${CMAKE_SOURCE_DIR}/src/heuristics/scroll_state.cpp
)

target_include_directories(test_heuristics PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_heuristics PRIVATE
    GTest::gtest_main
    Threads::Threads
)

# Project-strict flags break GoogleTest internals; relax them for tests
# while keeping the daemon target's discipline intact. We still keep
# -Wall -Wextra on our own test sources so they don't rot.
target_compile_options(test_heuristics PRIVATE
    -Wall
    -Wextra
)

# ─── scope tests ─────────────────────────────────────────────────────
# rules_loader.cpp pulls in util/log.hpp via LX_WARN / LX_INFO_C macros,
# so we link util/log.cpp too. No other heavy daemon TUs are dragged in
# (presets/config helpers used by the loader are header-only constexpr).
add_executable(test_scope_rules
    scope/test_rules.cpp
    ${CMAKE_SOURCE_DIR}/src/scope/rules.cpp
    ${CMAKE_SOURCE_DIR}/src/scope/rules_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/util/log.cpp
)

target_include_directories(test_scope_rules PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_scope_rules PRIVATE
    GTest::gtest_main
    Threads::Threads
)

target_compile_options(test_scope_rules PRIVATE
    -Wall
    -Wextra
)

include(GoogleTest)
gtest_discover_tests(test_heuristics)
gtest_discover_tests(test_scope_rules)
