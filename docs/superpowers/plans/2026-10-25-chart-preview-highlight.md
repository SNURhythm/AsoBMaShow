# Chart Preview Highlight Implementation Plan

**Goal:** Default chart previews to a fast estimated highlight with a short fade.
**Architecture:** Pure chart-timing heuristic feeds the existing paged source's
absolute start frame. An optional player fade counts consumed output frames.
**Tech Stack:** C++23, existing parser structures, CTest, existing choice controls.
**Spec:** ../specs/2026-10-25-chart-preview-highlight-design.md

## Constraints and review focus

No audio analysis, new dependencies, parser edits, or duration-sized allocations.
Keep beginning mode, cancellation, audio/visual clocks, and gameplay handoff intact.
Cover sparse/short charts, isolated bursts, sustained BGM, pending callbacks,
multiple output rates, and old settings without the new key.

## Tasks

- [x] Extend paged preview tests with parsed chart fixtures for highlight choice
  and fallback. Add `audio::FindChartPreviewHighlightMicros(const Chart &)` in
  `src/audio/ChartPreviewHighlight.h`, scoring sparse one-second activity bins
  at measure boundaries with a 12-second window.
- [x] Extend stream player tests for the optional 300 ms fade, buffering, restart,
  and output-rate independence. Add `bool fadeIn = false` to `Start`.
- [x] Persist `chartPreviewMode` (default Highlight) and
  `chartPreviewFallbackHighlight` (default true). Add Highlight/Beginning/Preview
  File choices and conditional fallback controls under BMS Library. Wire both
  selectors and reuse the existing authored-file player and custom preload PCM.
  Extend settings round-trip and worker integration coverage.
- [x] Build affected tests and desktop main in one build invocation; run audio,
  music-select, and relevant settings/localization checks. Review the diff, then
  commit and push this branch.

## Verification

- Desktop `main` compile and focused settings/layout/localization checks.
- All 34 `area_audio` tests, including source seeking, fade, PCM sharing,
  cache identity/budget behavior, and native Jukebox lifecycle coverage.
- Selector lifecycle, export/preload handoff, settings retention and preview
  routing checks; missing/usable authored files and both fallback choices.
- Regression checks for both selectors returning from 150% gameplay and for
  retained regular/ZIP/7z keysounds avoiding repeat decode/archive payload reads.

Independent review found the retained-PCM and gameplay-rate ordering issues;
regression tests reproduced them before the fixes. Authored files intentionally
have no synthesized chart BGA timeline because their audio is independently cut.

## Hybrid scoring follow-up

- [x] Extend `testHighlightSelection` in `tests/paged_preview_source_tests.cpp`
  with equal note/BGM weighting, a sustained entrance versus a slightly denser
  plateau, and a short burst that must not beat a sustained section. Run
  `paged_preview_source_tests` and confirm the first two regressions fail.
- [x] Update `FindChartPreviewHighlightMicros` in
  `src/audio/ChartPreviewHighlight.h` with equal activity weights and the
  bounded rise bonus from the spec, reusing prefix sums and one extra cursor.
  Preserve short/sparse, BGM-only and unkeyed-note coverage.
- [x] Build `main` and `paged_preview_source_tests` together at `-j 6`, run the
  focused CTest, update feature documentation, review the diff, commit and push.

Verification: unequal BGM weighting and missing entrance preference each failed
before their fixes. The parsed entrance fixture also exposed microsecond
truncation at a second boundary; nearest-bin scoring now selects the intended
measure. Desktop `main` and `paged_preview_source_tests` passed. Independent
review found no blocking issues; perceptual quality still needs listening.

## Bounded duration preparation follow-up

- [x] Add a deterministic regression to `tests/paged_preview_source_tests.cpp`
  that blocks an expired sound's PCM allocation and still expects highlight
  startup/current audio before the gate is released. Verify failure first.
- [x] Add `src/audio/AudioDurationBound.h`: bounded positioned reads of PCM WAV
  chunk headers and FLAC STREAMINFO, plus shared Vorbis page checks and bounded
  Ogg head/tail reads. Test payload-skipping byte counts, malformed metadata
  and conservative boundaries.
- [x] Add identity-checked `DecodedAssetCache::Find` and integrate probes into
  `PagedPreviewSource.cpp`. Reuse cached PCM; prioritize opening events, publish
  excluded past instances independently of their background PCM, preserve known
  long overlaps and cap unknown lookback at 15 seconds. Archive probes reuse one
  extraction pass with bounded deferred bytes.
- [x] Cover loose/ZIP/7z preparation, the unknown 15-second boundary and known
  long tails, cancellation/source identity, and one eventual decode per asset.
- [x] Build `main` and affected audio tests at `-j 6`, run `area_audio`, obtain
  focused review, document measured probe byte counts/timing and limitations,
  then commit and push to this branch.

Probe measurements: on this machine, 1,000 warm-filesystem open/probe/close runs
on a 128 MiB sparse WAV had median 34.5 microseconds and p95 68.167 microseconds.
Each requested 74 bytes. This excludes path resolution/cache identity checks and
does not represent cold storage, Android providers, or archive extraction time.
Large Ogg probes read at most 65,636 bytes, independent of total file length;
FLAC reads 42 bytes. WAV chunk counts and Ogg candidate scans are bounded.

Regression evidence: the expired-sound PCM gate failed before the optimization;
a later-reused ZIP sample also failed until exclusion became per past instance.
The gates now show startup before background decode and no resampling of cached
expired-only sounds, while future instances and known longer tails stay audible.

Final verification: desktop `main` and all 34 `area_audio` tests passed. Review
caught cached expired resampling, trailing RIFF content and a probe/decode source
identity gap; all were corrected. A repeated-unknown-sample regression then
failed until the 15-second cutoff was applied independently to each old onset.
The final targeted review found no actionable issues. Archive reads continue
through the indexed batch pipeline; no separate probe extraction was added.


## ZIP first-selection startup follow-up

The user reported about one second on iOS for the first selection of each chart
inside a 20 GiB ZIP, with the library index already built. The exact archive was
unavailable locally. A smaller real ZIP reproduced unnecessary work before
startup: 360/370 sounds decoded, compared with 29–36 from loose files. Sampling
showed ZIP inflation and CRC checks dominating; scoring took about 0.12 ms.

- [x] Extend the opt-in benchmark with `--highlight` (actual scored origin and
  leading-silence trimming), preserving the existing gameplay handoff checks.
- [x] Strengthen the expired ZIP regression to gate full encoded allocation,
  not only PCM allocation. Confirm failure before changing production code.
- [x] Add bounded ZIP metadata reads using the cached index/directory, then split
  existing batch extraction into opening/overlap and remaining assets. Preserve
  full extraction CRC checks, native cache warming, cancellation and identity.
- [x] Cover stored/deflate WAV, stored Ogg tails, deflate prefix limits, per-entry
  I/O budgets, cancellation, source replacement and future sample reuse.
- [x] Independently review the change; no actionable findings reported.

The initial optimized desktop comparison (48 kHz, one warm-up and three measured
rounds, warm filesystem/index but fresh decoded cache) reduced median ZIP startup
from 106.0 ms to 21.4 ms; only 23 sounds had decoded at early cancellation. Full
preparation rose from 109.6 ms to 116.8 ms due to the extra bounded probes and
second batch setup. The 44 MB/599-entry chart has 370 sound references. These are
headless preparation measurements, not iPhone or UI-to-audible latency claims.

A sparse 128 GiB ZIP with 100,004 entries also completed highlight preparation
and gameplay handoff. An instrumented run (warm-up plus one measured round,
three modes) counted 18,239,580 source bytes and zero reads from the unselected
128 GiB payload. Stored entries avoided mmap payload reads in that fixture.
This checks bounded access to a large archive; it does not reproduce the user's
20 GiB collection or cold iOS storage. See the focused review report for final
verification and reproduction.

Final checks: desktop `main` and the audio/archive targets built; all 53 selected
CTest cases passed, followed by the three focused tests after tightening the
input-budget cursor invariant. The unsigned iOS device build also passed. No
upload or device timing claim is part of this change.


## Unknown Ogg lookback follow-up

- [x] Retain Ogg container detection even when its bounded duration probe fails;
  use 60 seconds for unknown Ogg and preserve 15 seconds for other unknown formats.
  Known/cached longer durations and future onsets keep their existing behavior.
- [x] Add a real 62-second Ogg fixture named `.wav` inside stored/deflated ZIPs.
  Confirm the 59-second deflated case fails under the old 15-second policy,
  then verify the 59/60-second boundary and known/cached tails beyond one minute.
- [x] Build desktop `main` and pass the focused preview tests. The existing AIFF
  tests continue verifying the unchanged 15-second fallback for other formats.

This extends the overlap lookback, not preview playback duration or the bounded
ZIP probe budget. It may increase startup work when more old Ogg onsets now need
full extraction and PCM decoding; nominal bitrate estimates are not used.

The unsigned iOS device compile check also passed for this policy update.
