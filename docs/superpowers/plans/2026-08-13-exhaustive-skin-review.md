# Exhaustive Skin Review Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate source-compatible Lua host file-I/O edge failures and establish a repeatable, source-backed review path for skin and replay-export changes.

**Architecture:** Keep Beatoraja’s direct, live skin-file semantics as the compatibility baseline. Harden only behavior that has no valid upstream-compatible result (overflow and allocation derived from a request that exceeds actual readable bytes), then record a review matrix that distinguishes upstream parity, project-owned replay propagation, and platform lifecycle checks.

**Tech Stack:** C++23, LuaJIT host bindings, CTest, CMake, pinned Beatoraja source at `c2ed5db1a46145ed10790c3872f717e95b59db9d`.

## Global Constraints

- Do not add profile overlays, render-phase I/O denial, revision freezing, or arbitrary Lua file-size caps: pinned Beatoraja directly reads and writes the live selected skin directory.
- Preserve normal Lua `file:read(n)` behavior: return no more than the bytes left in the file, even when `n` is larger than the file.
- Preserve negative seek clamping to byte position zero; reject only positions unrepresentable by `std::streamoff`.
- Use `cmake-build-debug` incrementally with `-j 12`; do not run deployment/upload scripts.
- Preserve unrelated `android/build/` worktree content.

---

### Task 1: Make Lua file reads proportional to available bytes

**Files:**

- Modify: `tests/lua_skin_host_modules_tests.cpp`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.cpp:850-1020`

**Interfaces:**

- Consumes: Lua `file:read(number)` and `file:read("*a")` methods installed by `LuaSkinHostModules`.
- Produces: identical returned bytes for ordinary reads without allocation based on a numeric request larger than the unread file tail.

- [x] **Step 1: Write the failing runtime test**

Add an `io_large_read.luaskin` fixture that opens a three-byte file, calls `file:read(2147483647)`, and asserts that the returned value is exactly `"abc"`. Add `testIoReadClampsHugeRequestedCountToAvailableBytes`, which loads that configured runtime and asserts it succeeds.

- [x] **Step 2: Run the focused test to verify it fails**

Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests -j 12 && ./cmake-build-debug/lua_skin_host_modules_tests`

Expected: the configured runtime fails before the host read path limits the allocation to the unread file tail.

- [x] **Step 3: Implement the minimal host read helper**

Add one shared helper that obtains the unread regular-file byte count, allocates at most `min(requested, remaining)`, and performs the stream read. Reuse it for `file:read(number)` and `file:read("*a")`; map allocation or stream failures through the existing stored diagnostic path. Do not add a fixed byte cap.

- [x] **Step 4: Re-run the focused test**

Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests -j 12 && ./cmake-build-debug/lua_skin_host_modules_tests`

Expected: PASS, including the new huge-request behavior.

### Task 2: Make Lua seek arithmetic representable before using streams

**Files:**

- Create: `src/skin/beatoraja/LuaSkinFileIo.h`
- Modify: `src/skin/beatoraja/LuaSkinHostModules.cpp:1010-1055`
- Modify: `tests/lua_skin_host_modules_tests.cpp`

**Interfaces:**

- Consumes: a nonnegative `std::streamoff` base and signed Lua byte offset.
- Produces: `std::optional<std::streamoff>` that retains ordinary/clamped seek positions and returns empty for positive or conversion overflow.

- [x] **Step 1: Write the failing boundary test**

Include `LuaSkinFileIo.h` and add an expectation that `checkedSeekPosition(0, std::numeric_limits<std::int64_t>::min())` returns `0`, while `checkedSeekPosition(std::numeric_limits<std::streamoff>::max(), 1)` has no value.

- [x] **Step 2: Run the focused test to verify it fails**

Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests -j 12 && ./cmake-build-debug/lua_skin_host_modules_tests`

Expected: compile failure because the checked helper does not exist.

- [x] **Step 3: Implement the checked arithmetic helper and use it from `fileSeek`**

Define `skin::lua_file_io::checkedSeekPosition(std::streamoff, std::int64_t)` using comparison-before-addition. Require at compile time that `lua_Integer` is signed and no wider than `std::int64_t`, then report the existing file-operation error for an unrepresentable result.

- [x] **Step 4: Re-run the focused test**

Run: `cmake --build cmake-build-debug --target lua_skin_host_modules_tests -j 12 && ./cmake-build-debug/lua_skin_host_modules_tests`

Expected: PASS, with normal negative offsets still clamped to zero.

### Task 3: Record and execute the exhaustive review path

**Files:**

- Create: `docs/skin-compat/exhaustive-review-path.md`

**Interfaces:**

- Consumes: the pinned Beatoraja SHA, the current branch diff, and CTest/iOS verification outputs.
- Produces: a repeatable audit matrix with commands, required evidence, ownership boundaries, and a disposition rule for reviewer suggestions.

- [x] **Step 1: Write the audit matrix**

Document three required passes for any skin/replay change: (1) complete upstream source comparison for touched compatibility behavior, (2) normal/replay-watch/course-export authority propagation comparison, and (3) resource, lifecycle, and cross-platform verification. For every pass, include the exact local source root, command, expected evidence, and rule that an upstream-incompatible restriction is rejected rather than implemented.

- [x] **Step 2: Classify the current review batch**

Record the seek overflow as fixed, the enormous numeric read as fixed without an arbitrary cap, and the suggested read-only/profile-overlay restriction as rejected because Beatoraja’s `SkinLuaAccessor` permits direct live-directory writes and the user explicitly requires mid-session edits.

- [x] **Step 3: Run final verification**

Run: `cmake --build cmake-build-debug --target main -j 12 && ctest --test-dir cmake-build-debug --output-on-failure -j 1 && scripts/ios_release_verify.sh`

Result: focused Lua-host test passed; `main` compiled; the serialized full suite passed 260/260; unsigned iOS verification passed 61/61 release-critical tests, its four Python contract suites, arm64 build, and artifact audit. A parallel `-j 12` CTest run was observed to fail only the two pre-existing global-bgfx tests, while both passed in isolation and the serialized suite passed 260/260.

- [x] **Step 4: Commit and push only the reviewed files**

Committed `fix: audit Lua skin host file boundaries`; the push of
`feature/luaskin` is the final repository action. `android/build/` remains
untracked and excluded.

## Self-Review

- Spec coverage: Task 1 covers the huge-request allocation review; Task 2 covers the seek overflow review; Task 3 turns the current review triage into a repeatable local path.
- Compatibility coverage: the global constraints preserve direct live Files-directory reads/writes, as verified against pinned Beatoraja `SkinLuaAccessor.java`.
- Placeholder scan: no task uses TBD/TODO or a generic testing instruction.
- Type consistency: Task 2 defines the exact `checkedSeekPosition(std::streamoff, std::int64_t)` signature used by the host module and test.
