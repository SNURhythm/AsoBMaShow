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
