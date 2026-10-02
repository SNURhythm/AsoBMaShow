# Custom object parity implementation plan

> **For agentic workers:** Use superpowers:executing-plans inline with test-driven development.

**Goal:** Continue the requested review/fix loop for custom timers and event conditions.

**Architecture:** Reuse the existing LuaJ scalar conversions at each session's custom-object evaluation boundary. Preserve Standard-mode failure handling and carry the selected policy into the gameplay bridge. Review timer state lifetime separately before making any related change.

**Tech Stack:** C++20, LuaJIT, CMake/CTest, pinned local Beatoraja source.

**Spec:** User's “keep going”; Beatoraja `c2ed5db1a46145ed10790c3872f717e95b59db9d`, `SkinLuaAccessor.loadBooleanProperty/loadTimerProperty`, `CustomTimer`, and `CustomEvent`. Compatibility mode must coerce callback values as LuaJ does.

## Global constraints

- Current branch and checkout; no worktree or deployment.
- Preserve external skins and Lua file persistence; no LITONE-specific tests.
- No whole-file formatting; one build per build directory.
- Commit and push verified task changes to the current upstream.

## Review focus

- `0` and empty strings are truthy, while nil/false are false.
- Timer strings and fractional numbers use LuaJ long conversion, including saturation.
- Standard mode retains its existing strict callback admission.
- Policy reaches the gameplay bridge through actual session creation.
- Custom timer state and callback failures must be examined independently of successful-value conversion.

### Task 1: Callback conversion

**Files:** `tests/play_skin_session_tests.cpp`, `src/skin/beatoraja/PlaySkinStateBridge.{h,cpp}`, `PlaySkinSession.cpp`, and `ResultSkinSession.cpp`.

**Interfaces:** Existing `luaJToBoolean`/`luaJToLong` and session `SkinSafetyPolicy`; add a defaulted policy to `PlaySkinStateBridgeContext` if needed.

- [x] Add real-session cases for timer strings/fractions/nil, truthy numeric/string conditions, and false/nil conditions across gameplay, select, result, and course result.
- [x] Build `play_skin_session_tests` and observe the compatibility failures.
- [x] Apply shared conversion only when LuaDecoderLimit is disabled, preserving Standard behavior.
- [x] Rebuild and run session/bridge suites.

### Task 2: Adjacent review and delivery

- [x] Compare active/passive timer state lifetime, error defaults, and event dispatch with pinned source; reproduce actionable gaps before changing code.
- [x] Fix compatibility error defaults, session timer state, passive writes, and setter conversion after separate failing regressions.
- [x] Review the final diff and document bounded findings and test evidence.
- [x] Reproduce the independent review's photo-export timer mutation and isolate photo state; preserve video state.
- [x] Build all targets, run full CTest in parallel, and check the diff.

Report: `docs/skin-compat/2026-10-02-custom-object-review.md`.
Delivery: commit verified changes and push to the current upstream.
