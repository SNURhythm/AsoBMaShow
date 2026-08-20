# PR 99 Review Round Three Implementation Plan (Complete)

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Correct the five valid newly-opened PR 99 review findings while preserving the pinned gameplay-skin contracts.

**Architecture:** Extend the shared image decoder so its WebP fallback accepts the bounded byte source already used by Android SAF and virtual archives. Preserve a decoded legacy provenance's wire schema while serializing it, aggregate local chart and course attempts for PlayerData compatibility, and source replay-export metadata from the same repository/resource facts as live gameplay. Keep the gauge predicate aligned with the authoritative enum indexes.

**Tech Stack:** C++23, FFmpeg/libavformat, SQLite, CTest, Xcode build-only verification.

**Spec:** Five unresolved GitHub review threads fetched for PR 99 on 2026-08-20.

## Global Constraints

- Validate every reviewer claim against the current branch before changing code.
- Write and run a focused failing regression before each production fix.
- Commit each independently testable fix separately; leave `vcpkg_installed/` untracked.
- Resolve only validated GitHub review threads after their focused checks pass.

---

### Task 1: Decode WebP from bounded byte-backed resources

**Files:**
- Modify: `src/view/ImageFileDecoder.cpp`
- Test: `tests/image_file_decoder_tests.cpp`

- [x] Add a direct `decodeImageMemory` assertion for the existing FFmpeg-only two-by-two WebP fixture, including a one-pixel target.
- [x] Run `image_file_decoder_tests` and observe that the byte decoder rejects this WebP before the file-only fallback is reached.
- [x] Detect WebP container bytes after native/CIM/WBMP decoding fails and send the bounded memory source through an FFmpeg custom IO context, reusing the existing dimension, decoded-byte, stop-token, and resize guards.
- [x] Re-run `image_file_decoder_tests`, commit `fix: decode byte-backed WebP resources`, and resolve the thread.

### Task 2: Preserve legacy provenance wire schema

**Files:**
- Modify: `src/ScoreProvenance.cpp`
- Test: `tests/score_provenance_tests.cpp`

- [x] Add a v5 stage-provenance fixture by removing `playDurationSeconds` from a current serialized proof, then assert deserialize/serialize returns the original v5 JSON byte-for-byte.
- [x] Run `score_provenance_tests` and observe that the reserialized value is v6 and contains `playDurationSeconds`.
- [x] Choose `fingerprintSchemaVersion` as the serializer's wire schema when it is present, and suppress the v6-only stage duration field for older wire schemas.
- [x] Re-run `score_provenance_tests`, commit `fix: preserve legacy provenance wire schema`, and resolve the thread.

### Task 3: Include local course attempts in player history

**Files:**
- Modify: `src/repositories/ScoreRepositoryQueries.cpp`
- Test: `tests/score_provenance_db_tests.cpp`

- [x] Extend the existing PlayerData history fixture with a local `course_scores` aggregate and stage provenance duration, then assert play count, clear count, judgement totals, and duration include it.
- [x] Run `score_provenance_db_tests` and observe that the current snapshot includes only chart `scores` rows.
- [x] Merge local `scores` and `course_scores` aggregate facts; derive course duration by safely reading the persisted provenance stages without admitting imported chart rows.
- [x] Re-run `score_provenance_db_tests`, commit `fix: include course attempts in player history`, and resolve the thread.

### Task 4: Project chart metadata into replay-video gameplay authority

**Files:**
- Modify: `src/ReplayVideoExporter.cpp`, `src/ReplayVideoExporter.h`
- Test: `tests/replay_playfield_presentation_tests.cpp`

- [x] Add a testable chart-metadata authority helper and a fixture with a persisted SongReview bit, document marker, stage image, and back image.
- [x] Run its focused replay-presentation test and observe the export authority helper does not yet provide these facts.
- [x] Populate SongReview/document state from `ChartRepository` and stage/back availability through the same image-resource semantics as live gameplay; retain all four facts in normal and course initial and per-frame authority updates.
- [x] Re-run `replay_playfield_presentation_tests`, compile replay export consumers, commit `fix: carry chart metadata into replay export`, and resolve the thread.

### Task 5: Recognize the grade EX gauge indexes

**Files:**
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Test: `tests/play_skin_state_bridge_tests.cpp`

- [x] Add EX GRADE and EXH-CLASS states to the boolean-property-1046 fixture and assert both are true.
- [x] Run `play_skin_state_bridge_tests` and observe each state selects the false branch.
- [x] Include enum indexes 7 and 8 in the bridge's `gauge_ex` predicate, matching its pinned Beatoraja comment.
- [x] Re-run `play_skin_state_bridge_tests`, commit `fix: recognize grade gauges in skin gauge_ex`, and resolve the thread.

## Independent Self-Review

- [x] Inspect `origin/feature/skin-compat..HEAD` for correctness, stale comments, accidental API expansion, and cross-platform FFmpeg/resource handling.
- [x] Ask a fresh code-review agent to review the five commits against this plan and inspect each reported issue.
- [x] Fix each valid review finding in its own commit and repeat the focused verification: FFmpeg validates bounded WebP headers before probing (`32379031`, `cc5f2da2`), and replay metadata matches normalized chart storage paths (`e5f88512`).

## Final Verification

- [x] Run `cmake --build cmake-build-debug --target main -j 6`.
- [x] Run `ctest --test-dir cmake-build-debug --output-on-failure -j 6` (266/266 passed).
- [x] Run `scripts/ios_firebase_deploy.sh --build-only` (BUILD SUCCEEDED).
- [x] Confirm no valid unresolved PR 99 review threads remain, commit this completed plan separately, and push `feature/skin-compat`.
