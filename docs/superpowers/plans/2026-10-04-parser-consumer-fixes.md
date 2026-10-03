# Parser consumer fixes implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Resolve the confirmed parser-consumer gaps and document historical replay compatibility evidence.

**Architecture:** Keep parser behavior and generated artifacts owned by the upstream agent. Harden shared app consumers, adopt the verified parser API with persistent metadata invalidation, and establish replay compatibility from historical fixtures.

**Tech Stack:** C++23, CMake/CTest, SQLite, upstream amalgamated BMS parser.

**Spec:** `docs/audits/2026-10-04-bms-parser-api-handoff.md`

## Global constraints

- Work in this checkout; do not create a worktree or deploy.
- Do not edit generated parser files directly or disturb concurrent upstream changes.
- Preserve dates, favorites, scores, and replay evidence; never change mine damage.
- One build at a time in `cmake-build-debug`; no whole-file formatting.
- Keep changes local until the upstream parser patch arrives (user instruction); do not push this slice beforehand.

## Review focus

- Invalid timing at a timeline exactly on a beat must reject the whole generated plan.
- Large valid selected audio exports must retain existing support above 100000 beats.
- Cancellation must be checked even in measures producing no beats and in timeline traversal.
- Interrupted/offline metadata rebuilds must retain dates and pending state across launches.
- Historical replay mismatches must remain visible without rewriting original results.

## Task 1: Safe club timing

**Files:** `src/audio/ClubBeat.{h,cpp}`, `src/audio/Jukebox.cpp`, `src/audio/ChartAudioRenderer.cpp`, `src/ChartPlaybackDuration.h`, `tests/club_beat_tests.cpp`, `tests/chart_audio_renderer_tests.cpp`.

**Interface:** Keep `buildPlan` vector output; return an empty plan for unrepresentable timing, cancellation, or excessive work. Add an optional diagnostic status if export needs to distinguish invalid plans from empty charts. Reject nonfinite/negative durations, timestamps and beat positions; check conversion and additions before narrowing. Saturate the unused gameplay-end helpers separately.

- [x] Add parsed Infinity/large STOP regression, maximum timing plus beat offset, NaN/negative timing, tiny BPM, cancellation and excessive measure-scale tests; require empty plans with no partial output.
- [x] Run focused tests and UBSan to establish the failure.
- [x] Implement checked conversion/addition and finite bounded beat production, preserving ordinary tempo/STOP timing and the existing selected export above 100000 beats.
- [x] Verify focused tests and sanitizer probes; cover export failure without replacing an existing output file.

## Task 2: Mine recount

**Files:** `src/CoursePlaySession.h`, `tests/replay_playfield_presentation_tests.cpp`.

**Interface:** Preserve `applyEffectiveLongNoteModeToChart`; count unique actual mines from both supported containers separately from playable notes.

- [x] Add parsed mine plus ordinary/scratch/LN cases and an alias-container case; demonstrate lost mine count.
- [x] Deduplicate mines without changing damage or ordinary counters.
- [x] Build/run the preparation regression target.

## Task 3: Upstream adoption and LN graph

**Files:** generated `src/bms_parser.{hpp,cpp}`, `src/ArchiveFile.cpp`, `src/ChartLibraryScanner.cpp`, LN consumer files identified by the audit, corresponding archive/scanner/visual/gameplay tests.

**Interface dependency:** Upstream agent supplies verified commit, artifacts, byte-format API and LN/mine/numeric contracts. Finalize exact API call edits only after that handoff.

- [ ] Record commit and artifact hashes; reproduce old PMS and exact malformed-LN cases before copying artifacts.
- [ ] Propagate actual inner/document filename format through buffered Parse/Scan callers.
- [ ] Test PMS path/bytes/archive parity including uppercase extension, lanes, links and metadata; preserve ordinary BMS behavior.
- [ ] Apply consistent malformed-LN policy matching the upstream contract to visual model, judging/counting and export paths; exercise orphan endpoints and null timelines under ASan/UBSan.

## Task 4: Persistent metadata revision

**Files:** `src/repositories/ChartRepository.cpp`, `src/ChartLibraryScanner.cpp`, `tests/chart_repository_tests.cpp`, `tests/chart_library_scanner_tests.cpp`.

**Interface:** Reuse `invalidateChartMetadataForNormalScan(db, completed, true)` and rebuild-required state; tie invalidation to the adopted parser semantic revision.

- [x] Add schema-12 upgrade regression with unchanged ordinary/archive sources, retained dates, offline/interrupted/scoped scans and second-launch idempotence.
- [x] Implement a durable revision/migration using the existing rebuild path; retain pending preserved dates during retries.
- [x] Run repository/scanner targets and compare fresh Scan output with persisted metadata.

## Task 5: Historical replay evidence and verification

**Files:** replay setup/consumer/materializer and tests as required by the historical comparison; audit handoff.

- [ ] Capture fixtures produced with the prior parser, then compare with the pinned parser: legacy/modern, PMS, timing/STOP, LN/LNOBJ, RANDOM and course stages.
- [ ] Trace Watch/Retry Same/G-Battle/practice/course/export diagnostics; document and test compatibility decisions without claiming equivalence from hashes.
- [ ] Run `cmake --build cmake-build-debug -j 6`, then `ctest --test-dir cmake-build-debug --output-on-failure -j 6` plus focused sanitizer probes.
- [ ] Review changes, update audit with exact revisions, evidence and remaining limitations, commit locally and push only after the upstream parser patch arrives.

## Execution record

- Starting app commit: `68382de1627f321aa8a56c7a961d75b4f7b974b7`.
- Upstream remains owned by another agent, confirmed by user; current observed HEAD `14d7a23` is not the adoption revision.
- Proceed inline under the user's implementation request; API-dependent work waits for upstream coordination while independent app fixes proceed.

### Independent slice review

- Findings 1, 4 and 5 implemented. Tasks 3 and 5 remain open pending the upstream handoff.
- UBSan (including float-cast-overflow and signed-integer-overflow) and ASan/UBSan pass for the expanded club-beat/helper tests.
- Full desktop/native build passed. Full CTest passed 419/419 tests, zero failures (255.07 seconds).
- Independent review found no blocking defects. Optional test coverage deferred: cancellation flipping during a timeline-only or zero-beat traversal (checks are present; pre-cancellation is tested).
- Ruling: schema 13 must ship with final parser adoption; if released separately, adoption needs another metadata invalidation. This branch has not been deployed.
- Ruling: reject invalid club plans as a whole and cap generated beats at 1,000,000. This disables generated club beats for extreme charts and rejects affected audio exports; existing selected exports above 100,000 beats remain supported.
- Ruling: retain existing replay behavior until historical fixtures and the final parser contract support a compatibility decision. Remaining warning-visibility gaps are documented in the handoff.

- Latest user instruction: do not push before the upstream parser patch arrives. Local verification/commits may proceed; the push is on hold.

- Local implementation commit: `b64bf3d4`. Code and regressions are verified; no push performed. The remaining adoption/replay tasks are open.
