# Practice and analysis

## Intent and user flow

Practice lets a player choose a chart section, persist chart-specific presets,
apply eligible rule overrides, loop the selected range, vary playback rate, and
inspect replay-backed timing analysis after play. Entry points live in the chart
viewer, gameplay, results, and replay flows so the same section can be resumed
without changing its chart-time meaning.

The chart viewer's **View source** button opens the original chart text in a
full-page, read-only overlay using the chart loader's encoding detection.
On iOS, Android, macOS and Windows, the text uses the operating system's native
selection and copy controls. Manual text selection stays independent of practice
highlights; copying never modifies the source file.
Syntax colors distinguish directives, control flow, resource/channel identifiers,
numbers, text, channel data and comments. The viewer scans decoded source once
when opened and colors only bounded visible regions, reusing cached tokens while
scrolling or changing themes. Chart parsing and library scanning are unchanged.
Switching focus to another application and back preserves the open source viewer,
its scroll position and native selection.
**Highlight selection** marks data cells in the practice range and jumps directly
to the first match in source order, without scrolling animation. It uses the
loaded chart's BPM, STOPs, measure lengths, and random choices, with an exclusive
range end. Empty (`00`) cells
remain eligible so selected rests can be inspected too. Close, Escape, or
gamepad back returns to the viewer without moving practice markers.

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

Chart Viewer releases its listening audio through guarded cleanup before chart
and view destruction, including direct deletion and initialization failure.
Active, loaded, and retained listening resources require a stop; unused viewers
leave the shared jukebox untouched.

## Verification

Use `practice_configuration_tests`, `practice_session_tests`,
`practice_rule_override_tests`, `practice_analytics_tests`,
`practice_result_*_tests`, `gameplay_practice_*_tests`, and
`chart_viewer_lifecycle_tests`. Source display and selection use
`chart_source_tests` and the document-reader/overlay cases in
`text_input_box_tests`. Native Unicode ranges use `native_document_tests`;
`native_document_apple_tests` and `native_document_windows_tests` exercise their
desktop text controls when built on the corresponding platform. `bms_syntax_tests`
covers the standalone lexer and bounded lookup; the [syntax performance report](../testing/source-syntax-performance-2026-10-24.md)
records large-text timing and retained native attribute bounds.

## Related pages

- [Gameplay and scoring](gameplay-and-scoring.md)
- [Replays and video export](replays-and-video-export.md)
- [Results, records, and persistence](results-records-and-persistence.md)
