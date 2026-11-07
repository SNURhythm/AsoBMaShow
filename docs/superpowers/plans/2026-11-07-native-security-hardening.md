# Native Security Hardening Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement task-by-task.

**Goal:** Close the confirmed skin native-access boundary and patch vulnerable native dependencies.

**Architecture:** Keep the existing Lua host API, enforce source-only loading, and disable LuaJIT FFI in every target and cross-build tool. Use narrowly scoped vcpkg overlays for dependency updates rather than moving the entire baseline.

**Tech Stack:** C++20, LuaJIT, vcpkg, CMake/CTest.

**Spec:** User request: fix the security findings in a new branch and compare Lua behavior with `~/workspace/SNURhythm/beatoraja`.

## Global Constraints

- Branch `fix/native-security-hardening` in the current checkout; no new worktree.
- Preserve Beatoraja source-script behavior and document VM-specific differences.
- No deployment; commit and push verified task changes only.
- Use existing test targets and small fixtures; no whole-file formatting.

## Review Focus

- Valid binary chunks must be rejected at entry, module, dynamic-load, and callback boundaries.
- LuaJIT-only numeric syntax must not make FFI available; ordinary Lua source remains functional.
- FFI settings must agree across Windows, Unix, and cross-build host tools.
- IMA ADPCM frame counts must widen before multiplication; test virtual large input without allocating its payload.
- Dependency ports must build with the pinned vcpkg tool and retain existing platform features.

## Tasks

- [x] Extend `tests/lua_skin_runtime_tests.cpp` with harmless binary-load rejection and source-language checks; observe failure before changing production code.
- [x] Change all skin `luaL_loadbuffer` calls to text-only loading; disable FFI in `vcpkg-overlays/luajit` for target and host tools. Run existing Lua tests.
- [x] Add an IMA ADPCM frame-count regression to the audio tests; observe failure against the existing dependency. Select a complete upstream libsndfile snapshot and rerun.
- [x] Import pinned curl/OpenSSL/libarchive ports and refresh affected iOS device/simulator XCFrameworks.
- [x] Review embedded sources: update miniaudio, stb_image, PCRE2 and miniz; check SQLite. Use complete upstream versions and small compatibility adaptations instead of manual security backports, as requested.
- [x] Verify resolved versions and build affected subsystem groups plus desktop `main` and unsigned iOS app.
- [x] Review Beatoraja compatibility, source diff, and test outcomes; prepare the verified branch for commit and push.

## Reference findings

Beatoraja checkout `82365b8cd8d54f401bc9ea9f4b9e82d105ef7f92`, `SkinLuaAccessor.java`: standard and sandbox globals use LuaJ, remove unrestricted Java/native loaders, and install restricted IO. `execFile` accepts `bt` in LuaJ's format. LuaJIT's binary format and FFI are not part of this compatibility contract; source-only loading is a deliberate native-runtime security difference.

Dependency provenance, applicability, and compatibility changes are recorded in
`docs/reviews/2026-11-07-native-security-hardening.md`.

## Follow-up review and fixes

- [x] Trace additional font/media advisories against actual linked code and API parameters.
- [x] Reproduce FreeType COLR allocation overflow with synthetic bounds and an allocator that refuses allocations; update the complete upstream snapshot and SDL build integration.
- [x] Reproduce secondary-file access through disguised media; restrict all FFmpeg input boundaries to the selected file and retain normal-media coverage.
- [x] Complete rendering/video tests, desktop and unsigned iOS builds, and independent review; address FIFO input and FreeType visibility findings.
- [x] Commit and push the SDL dependency branch and parent PR update.
