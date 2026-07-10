# CTest Registration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make one CTest command build-system-aware of every existing AsoBMaShow test executable and Yoga's discovered GoogleTests.

**Architecture:** Enable CTest at the repository root, keep test creation behind both `ASOBMASHOW_BUILD_TESTS` and `BUILD_TESTING`, and register the thirteen existing standalone test executables with a small CMake helper. When tests are disabled, include only Yoga's `yogacore` subdirectory; when enabled, include Yoga's root so its existing `gtest_discover_tests(yogatests)` results are visible from the main build directory.

**Tech Stack:** CMake 3.22+, CTest, existing standalone C++23 test executables, Yoga GoogleTest suite.

## Global Constraints

- Work only on a task branch created from `feature/player-foundations`; never commit feature work to `develop`.
- Preserve `ASOBMASHOW_BUILD_TESTS=OFF` and standard `BUILD_TESTING=OFF` configurations without creating test targets.
- Do not modify the Yoga submodule.
- The canonical command is `ctest --test-dir cmake-build-debug --output-on-failure -j 6`.
- The current baseline builds successfully, but root CTest reports `No tests were found!!!`.

---

### Task 1: Register the existing suites with root CTest

**Files:**
- Modify: `CMakeLists.txt:1-8`
- Modify: `CMakeLists.txt:463-610`

**Interfaces:**
- Consumes: the existing `ASOBMASHOW_BUILD_TESTS` option and existing executable target names.
- Produces: CTest entries named exactly after the thirteen standalone executable targets; Yoga's existing discovered tests become visible through root CTest.

- [ ] **Step 1: Confirm the failing baseline**

Run:

```bash
ctest --test-dir cmake-build-debug --show-only
```

Expected: output ends with `Total Tests: 0` or `No tests were found!!!`.

- [ ] **Step 2: Enable CTest at the root**

Add immediately after the root `project(...)` call:

```cmake
include(CTest)
option(ASOBMASHOW_BUILD_TESTS "Build lightweight unit tests" ON)
```

This initializes the root `CTestTestfile.cmake`, defines the standard `BUILD_TESTING` option, and makes the project-specific switch available before subdirectories are selected.

- [ ] **Step 3: Select Yoga core or Yoga tests using the same switches**

Replace:

```cmake
add_subdirectory(bgfx)
add_subdirectory(src)
add_subdirectory(yoga)
```

with:

```cmake
add_subdirectory(bgfx)
add_subdirectory(src)
if (ASOBMASHOW_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(yoga)
else()
    add_subdirectory(yoga/yoga)
endif()
```

This continues to create `yogacore` for the application while omitting Yoga's test executable and GoogleTest download when tests are disabled.

- [ ] **Step 4: Make project test creation honor both switches**

Replace:

```cmake
if (ASOBMASHOW_BUILD_TESTS)
```

with:

```cmake
if (ASOBMASHOW_BUILD_TESTS AND BUILD_TESTING)
```

- [ ] **Step 5: Register every standalone executable**

Insert this block after the final `target_link_libraries(view_layout_tests ...)` line and before the matching `endif()`:

```cmake
    function(asobmashow_register_test target_name)
        add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
        set_tests_properties(${target_name} PROPERTIES
            WORKING_DIRECTORY $<TARGET_FILE_DIR:${target_name}>
        )
    endfunction()

    foreach(test_target IN ITEMS
        prep_metronome_tests
        gbattle_tests
        replay_summary_list_tests
        replay_record_filters_tests
        replay_db_helper_tests
        chart_record_filters_tests
        main_menu_library_tests
        chart_meta_index_order_tests
        score_cache_query_tests
        app_database_initializer_tests
        chart_filter_sort_panel_view_tests
        dropdown_view_tests
        view_layout_tests
    )
        asobmashow_register_test(${test_target})
    endforeach()
```

- [ ] **Step 6: Reconfigure and verify discovery**

Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j 6
ctest --test-dir cmake-build-debug --show-only
```

Expected: configuration and build succeed; the listing includes all thirteen names above plus Yoga tests and does not say `No tests were found`.

- [ ] **Step 7: Run the canonical suite**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: all registered tests pass. Existing Yoga skips remain skips, not failures.

- [ ] **Step 8: Verify disabled-test configurations**

Run:

```bash
cmake -S . -B /tmp/asobmashow-no-tests -DASOBMASHOW_BUILD_TESTS=OFF -DBUILD_TESTING=OFF
cmake --build /tmp/asobmashow-no-tests --target main -j 6
ctest --test-dir /tmp/asobmashow-no-tests --show-only
```

Expected: `main` builds and CTest lists zero tests; no standalone test target is created.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt
git commit -m "test: register suites with CTest"
```
