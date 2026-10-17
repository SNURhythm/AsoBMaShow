# Test artifact disk usage — 2026-10-17

The desktop Debug build repeated full variable/type debug information across
thousands of test object files. Before this change, the existing build directory
occupied approximately 11 GiB. Active objects accounted for 6.53 GiB, and another
1.13 GiB of object files were no longer referenced by the Ninja build graph.
The 407 top-level executables, including the application, totaled 1.92 GiB.

Registered Clang tests now use `-gline-tables-only` in Debug and RelWithDebInfo;
GCC uses `-g1`. This retains line information for source breakpoints and
symbolized stacks while omitting full local-variable/type inspection. Test
assertions remain enabled. Desktop Apple test links also use `-dead_strip` to
remove unused code. Application flags and MSVC debug information are unchanged.

To restore full debug information when inspecting test variables:

```sh
cmake -S . -B cmake-build-debug -DASOBMASHOW_TEST_FULL_DEBUG_INFO=ON
cmake --build cmake-build-debug --target play_skin_session_tests -j 6
```

Set the option back to `OFF` and rebuild to restore compact test objects.

## Local results

| Artifact | Before | After |
| --- | ---: | ---: |
| Complete build directory (`du -sh`, rounded) | 11 GiB | 5.5 GiB |
| Active object files (4,442) | 6.53 GiB | 2.79 GiB |
| Top-level executables (407, including the application) | 1.92 GiB | 1.54 GiB |
| `play_skin_session_tests.cpp.o` | 28.0 MiB | 7.7 MiB |
| `play_skin_session_tests` executable | 47.3 MiB | 40.3 MiB |

The local cleanup removed 882 obsolete object files (1.13 GiB). Each path was
checked against the current Ninja output graph before deletion, restricted to
`.o` files beneath `cmake-build-debug/CMakeFiles`. Current objects and unrelated
caches were retained. The reproducible compiler/linker changes save about
4.1 GiB in this build; the one-time cleanup reclaimed another 1.13 GiB.

The complete build succeeded, and all 498 CTest entries passed with six slots
and `CCACHE_DISABLE=1` in 73.07 seconds on the first run after rebuilding.
This verifies behavior, not a new runtime improvement. LLDB resolved a source
breakpoint in `play_skin_session_tests.cpp` with compact debugging enabled.
An incremental build after cleanup succeeded without recompiling test objects;
only routine application build-identity and shader work remained. `git diff
--check` passed. GCC/MSVC builds were not run in this macOS check.
