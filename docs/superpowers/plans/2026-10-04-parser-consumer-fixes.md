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
- Keep the application local under the user's explicit push hold, including after upstream arrival. Upstream-only parser commits/pushes follow AGENTS.md.

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

- [x] Record commit and artifact hashes; reproduce old PMS and exact malformed-LN cases before copying artifacts.
- [x] Propagate actual inner/document filename format through buffered Parse/Scan callers.
- [x] Test PMS path/bytes/archive parity including uppercase extension, lanes, links and metadata; preserve ordinary BMS behavior.
- [x] Apply consistent malformed-LN policy matching the upstream contract to visual model, judging/counting and export paths; exercise orphan endpoints and null timelines under ASan/UBSan.

## Task 4: Persistent metadata revision

**Files:** `src/repositories/ChartRepository.cpp`, `src/ChartLibraryScanner.cpp`, `tests/chart_repository_tests.cpp`, `tests/chart_library_scanner_tests.cpp`.

**Interface:** Reuse `invalidateChartMetadataForNormalScan(db, completed, true)` and rebuild-required state; tie invalidation to the adopted parser semantic revision.

- [x] Add schema-12 upgrade regression with unchanged ordinary/archive sources, retained dates, offline/interrupted/scoped scans and second-launch idempotence.
- [x] Implement a durable revision/migration using the existing rebuild path; retain pending preserved dates during retries.
- [x] Run repository/scanner targets and compare fresh Scan output with persisted metadata.

## Task 5: Historical replay evidence and verification

**Files:** replay setup/consumer/materializer and tests as required by the historical comparison; audit handoff.

- [x] Capture fixtures produced with the prior parser, then compare with the pinned parser: legacy/modern, PMS, timing/STOP, LN/LNOBJ, RANDOM and course stages.
- [x] Trace Watch/Retry Same/G-Battle/practice/course/export diagnostics; document and test compatibility decisions without claiming equivalence from hashes.
- [x] Run `cmake --build cmake-build-debug -j 6`, then `ctest --test-dir cmake-build-debug --output-on-failure -j 6` plus focused sanitizer probes.
- [x] Review changes, update audit with exact revisions, evidence and remaining limitations, commit locally; the user's application push hold remains in force.

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

## Continuation after verified upstream handoff

User supplied `bb8658a3d68f06c11b4ce4ff0d6c9b91055f2961` and explicitly retained
**the app push hold**. Base app revision is `e51d50d2`; completed fixes stay intact.
The upstream checkout is clean and its two generated artifact hashes match the
handoff. PMS support still requires an upstream API addition before final copy.

- [x] PMS/API worker: add compatible byte Parse/Scan source-filename overloads upstream, reproduce/test path-buffer parity, run required clean/modular/amalgamation tests, commit/push upstream only, copy both generated artifacts, propagate inner/SAF logical filenames through ArchiveFile, scanner and PlayOptionUtils.
- [x] LN/numeric worker: preserve raw parser ownership/identity; add `ChartPlayability.h` validation, follow local beatoraja LN behavior, including direct traversal of detached partners, harden integer STOP/interpolation arithmetic via `ChartTiming.h`, and bound prep-metronome work. Prove exact tiny-scale LN and extreme numeric behavior with native tests and sanitizers.
- [x] Replay worker: capture actual historical inputs and judged results against immutable old parser source; compare identical bytes with adopted parser across legacy/modern, PMS, STOP/fractional timing, LN/LNOBJ, RANDOM and course stages. Saved-result consumers reject result mismatches; lower-level materialization retains diagnostic evidence.
- [x] Integrator: validate file/buffer preparation and direct audio-export admission, preserve useful failure diagnostics, run all requested app builds serially, integrate tests and review all changes.
- [x] Complete the required full app/native build and parallel CTest suite, focused ASan/UBSan probes, final review, updated evidence/limitations and local commits. **Do not push the app.**

Implementation rulings:

- User steering: follow `/Users/xf/workspace/SNURhythm/beatoraja` for LN behavior.
  Detached partners are followed directly and their identities remain intact.
  Reference null-partner dereferences throw; report an invalid-graph diagnostic
  safely for that case, with no invented tap normalization or changed counts.
- Numeric policy uses representability checks and bounded work, without an
  arbitrary chart-duration ceiling. Direct numeric helpers must remain safe
  when given saturated parser values.
- Saved replay actions fail on materialized result disagreement. Original
  saved evidence stays immutable. Passing result checks establishes result
  reproduction, not unqualified parser/audio equivalence; historical fixtures
  separately measure note lookup and keysound behavior.
- Parallel workers own separate domains; only the integrator invokes the
  shared CMake/Ninja build directory.

Final continuation record:

- Adopted generated artifacts from upstream `5c3bb2f`, the tested buffered
  source-filename API addition on verified `bb8658a3`. Upstream committed/pushed
  separately; exact artifact hashes are in the audit.
- Full desktop/native build passed; a `main` no-op check performed no compilation
  or linking. Final parallel CTest passed **420/420**, zero failures, **116.51 s**.
- Focused ASan/UBSan, historical corpus/manifest checks and eleven actual Java
  decoder/SongInformation probes passed. These do not establish full Java
  JudgeManager parity or device/provider behavior.
- Historical buffered PMS and detached-LN results can disagree; saved playback
  rejects incompatible materialization while preserving original evidence.
  Detached live graphs remain supported. Lane/time-only replay adapters still
  cannot represent emitted detached identities; this limitation is explicit.
- Implementation and fixtures committed locally as `d5dd5177`. Documentation
  records the actual evidence and remaining limits. **Application push remains
  on hold; no deployment performed.**
