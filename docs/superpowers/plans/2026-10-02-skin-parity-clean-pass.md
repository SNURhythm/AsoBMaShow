# Skin parity clean-pass plan

> **For agentic workers:** Use superpowers:executing-plans with test-driven debugging and independent review.

**Goal:** Continue the user's review/fix loop until a fresh pass over the supported paths finds no further actionable problems.

**Reference:** AsoBMaShow baseline `3af57ca4`; local Beatoraja initially `c2ed5db1a46145ed10790c3872f717e95b59db9d`, updated at the user’s request to `ad42f56c4658e968f93b24bf23440fe51cb9878e`; verify final behavior against the updated source and bundled LuaJ jar.

**Constraints:** Current checkout and branch; no worktrees, external skin edits, Lua persistence changes, LITONE-specific tests, or deployment. Preserve formatting and run only one build per build directory. Commit and push verified changes to the current upstream.

## Review areas

- Lua host argument conversion, callback construction, and utility state.
- Main-state property bridges and writable audio state across gameplay, selection, result, and course result.
- Destination evaluation, renderer state, and supported image/text behavior.
- Score/rate projections and course/result state transitions.

## Work

- [x] Reproduce audio playback volume and HTTP timeout conversion differences with real host calls and captured backend arguments; fix confirmed differences.
- [x] Reproduce direct main-state volume setters through actual sessions, trace existing audio mutation paths, and fix confirmed missing bridge behavior while preserving frame/export lifetime rules.
- [x] Complete independent renderer review; reproduce and fix confirmed findings.
- [x] Review the remaining areas and re-review changed paths until a pass reports no further actionable issues.
- [x] Build all targets, run parallel CTest, check the diff, and document results and coverage limits.
- [x] Commit and push completed, verified work.

Each finding must have source evidence and a failing regression before its production fix. Re-review and repeat affected verification after follow-up fixes. A clean pass establishes the reviewed coverage, not exhaustive equivalence for all external skins.

### Task 3: Runtime-hidden text and font blend

**Owner files:** `src/skin/beatoraja/Skin2DRenderer.cpp`, `tests/skin_draw_command_tests.cpp` only. Coordinate with root before editing any other file.

**Reference:** Pinned Beatoraja above, `SkinText.prepare`, `SkinTextFont.draw`, `SkinTextBitmap.draw`, `SkinObject.draw`, and `Skin.SkinObjectRenderer`.

- Add a regression where retained text has a runtime draw condition that resets a shared flag and returns false; its value callback sets that flag; a following image's draw condition reads the flag. Upstream invokes the hidden text callback and draws the image. Cover timer-hidden text too. Preserve constructor-pruned numeric/static options and empty destination pruning.
- Fix value callback evaluation order only as supported by the source; preserve draw suppression and compatibility error defaults. Existing Standard safety behavior must retain its contract unless the test/source proves that a shared correction is appropriate.
- Add command-level regressions for scalable and bitmap font text after an additive image. The font text uses the retained renderer blend, independent of its authored blend; LR2 image fonts use their ordinary image draw path.
- Implement both findings and inspect adjacent object paths for which actual draw calls update retained blend. Avoid speculative refactoring.
- Observe failing tests before production changes. Ask root to run the focused build/test command at each RED/GREEN point; root owns the build directory lock.
- Do not edit external skins, Lua persistence, or LITONE-specific fixtures. Do not commit or push; root integrates and validates the combined change.
- Report source evidence, files, RED/GREEN evidence, and remaining concerns in the task report file. Do not spawn subagents.
