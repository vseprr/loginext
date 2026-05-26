# LogiNext Unit Tests

Pure-logic unit tests using GoogleTest. Covers the heuristics engine and
the per-app rule table — the two modules with pure logic that can be
exercised without a real device or compositor.

## Build

```bash
cd /path/to/loginext
cmake -B build -DBUILD_TESTS=ON
cmake --build build
```

The `BUILD_TESTS=ON` flag triggers a FetchContent download of GoogleTest
v1.14.0 the first time you configure. Subsequent builds reuse the cached
copy under `build/_deps/`.

Without `-DBUILD_TESTS=ON` (the default), nothing GoogleTest-related is
downloaded or compiled — the production daemon build is unaffected.

## Run

```bash
cd build
ctest --output-on-failure
```

Individual executables can also be run directly:

```bash
./build/tests/test_heuristics
./build/tests/test_scope_rules
```

Both link the corresponding production source files directly, so changing
`src/heuristics/scroll_state.cpp` automatically rebuilds and re-runs the
heuristics tests.

## Coverage scope

Initial coverage is intentionally illustrative rather than exhaustive —
this is the first wave of a test infrastructure rollout, and the goal is
to establish the build plumbing + a representative sample of tests per
module. Subsequent waves can add cases without further CMake work.

Modules currently under test:

- `src/heuristics/scroll_state.{hpp,cpp}` — gesture-start confirmation,
  idle reset, reverse-tick jitter debounce, leak decay.
- `src/scope/rules.{hpp,cpp}`, `src/scope/rules_loader.{hpp,cpp}`,
  `src/scope/app_hash.hpp` — FNV-1a case-insensitivity + sentinel, open-
  addressing table insert/lookup, load-factor cap, text-format parser.

## Adding new tests

- New `.cpp` test files: add the path to the relevant `add_executable()`
  call in `tests/CMakeLists.txt`. `gtest_discover_tests()` picks them up
  automatically.
- New modules: add a new `add_executable()` block in
  `tests/CMakeLists.txt` listing the test sources + the production source
  files under test. Each test target stays independent — link only the
  production TUs the test needs.
