# PR 115 branch review implementation plan

> **For agentic workers:** Use systematic debugging and test-driven fixes; root owns the build directory and integration. Reviewers work independently, then cross-review fixes.

**Goal:** Review the complete branch, correct confirmed defects, and repeat review until no actionable finding remains in the inspected paths.

**Architecture:** Keep each correction within its existing subsystem. Isolate desktop picker worker lifetime from application services, preserve fatal Lua failure classification, and suppress numeric draws that the reference renderer does not issue.

**Tech stack:** C++20, LuaJIT, bgfx, Python reference tooling, CMake/CTest.

**Spec:** User request to start a branch review loop using subagents with efficient reasoning levels. Review range: `deb6b394da2e8ff8bd330df273bb1a6f85d7f086..01e40867bf38bc6b5daf89c14daad0afda198f66`.

## Constraints and review coverage

- Current checkout; no worktrees, deployment, external skin edits, Lua persistence changes, or LITONE-specific fixtures.
- One Ninja/CMake build at a time; preserve surrounding formatting; commit and push verified task changes.
- Medium reasoning for onboarding/platform and tooling review; high reasoning for Lua semantics, rendering, and session/result lifecycle.
- Review shutdown during a native picker, late picker completion after service destruction, fatal versus recoverable callback setup failures, transparent nested draw state, and session/export isolation. The first four have explicit regressions below; the final area receives independent source review and existing session tests.

## Fixes

### 1. Desktop picker shutdown

Files: `src/library/ChartLibraryPlatform.cpp`, `tests/chart_library_operations_tests.cpp`.

- [x] Add a blocking native-picker stub regression: service destruction completes before dialog release; a late selection cannot register or enqueue a folder.
- [x] Observe the shutdown assertion fail before production edits.
- [x] Keep desktop worker result state independently owned; publish folder actions through live service polling. Teardown abandons pending results and does not join the uncancellable dialog.
- [x] Run `chart_library_operations_tests`, review lifetime/cancellation boundaries, and compile affected application paths.

### 2. JSON callback quota failures

Files: `src/skin/beatoraja/JsonGameplaySkinDecoder.cpp`, `tests/json_gameplay_skin_decoder_tests.cpp`.

- [x] Reproduce a Standard-mode JSON callback factory exhausting the real runtime quota while the decoder incorrectly accepts a partial model.
- [x] Preserve fatal classification using the existing Lua binding failure policy; retain recoverable malformed-script fallbacks.
- [x] Run JSON decoder and actual session-loading regressions; independently review fatal propagation.

### 3. Transparent nested numeric blend

Files: `src/skin/beatoraja/Skin2DRenderer.cpp`, `tests/skin_draw_command_tests.cpp`.

- [x] Reproduce a transparent additive song-list level changing the blend of following ordinary font text and retained state.
- [x] Suppress the numeric draw consistently with the reference alpha-zero path.
- [x] Run renderer/session/backend regressions and independently review nested draw ordering.

## Completion

- [x] Complete all production-domain and build/reference-tooling review passes; resolve any additional confirmed findings.
- [x] Re-review final changes, build affected targets, run the full parallel CTest suite, and check the diff.
- [x] Record findings, fixes, evidence, and remaining coverage limits; commit and push to the current upstream.
