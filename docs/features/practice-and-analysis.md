# Practice and analysis

## Intent and user flow

Practice lets a player choose a chart section, persist chart-specific presets,
apply eligible rule overrides, loop the selected range, vary playback rate, and
inspect replay-backed timing analysis after play. Entry points live in the chart
viewer, gameplay, results, and replay flows so the same section can be resumed
without changing its chart-time meaning.

## Code map

- `src/practice/PracticeConfiguration.*`, `PracticeSession.*`, and
  `PracticeLaunchRequest.*` model request and runtime state.
- `PracticePresetStore.*`, `PracticeAnalytics.*`, and result model/flow files
  own saved presets, analysis, and result handoff.
- `src/scene/Practice*` and `ChartViewerScene.*` present controls and launch
  requests; `src/scene/play/` consumes them during gameplay.

## Boundaries and invariants

Practice ranges are in original chart time. Audio rate implementation may
change output timing but must preserve that authority for charts, replay input,
and saved presets. Overrides are evaluated through the shared ruleset policy;
practice state must be identified in provenance so it is not confused with an
ordinary eligible score.

## Verification

Use `practice_configuration_tests`, `practice_session_tests`,
`practice_rule_override_tests`, `practice_analytics_tests`,
`practice_result_*_tests`, and `gameplay_practice_*_tests`.

## Related pages

- [Gameplay and scoring](gameplay-and-scoring.md)
- [Replays and video export](replays-and-video-export.md)
- [Results, records, and persistence](results-records-and-persistence.md)
