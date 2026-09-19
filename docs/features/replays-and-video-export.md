# Replays and video export

## Intent and user flow

Players can record an attempt, browse it in Records, watch or practice against
it, transfer it with a profile, and export playback to video. The stored format
is Beatoraja-compatible and is validated before it can drive playback or be
associated with a result.

## Code map

- `src/replay/ReplayFormat.*`, `BeatorajaReplayCodec.*`, and supporting
  primitives encode/decode the durable format.
- `ReplaySetup*`, `ReplayPlayback*`, `ChartReplay*`, and `CourseReplay*`
  prepare and consume verified playback data.
- `ReplayFileStore.*`, `ReplayFileLifecycle.*`, association, reconciliation,
  and profile-transfer services own file lifecycle and durable linkage.
- `scene/MusicSelectSceneRecords.cpp` contains Music Select's modal wiring and
  scene handoffs.
- `scene/ResultRecordsLoader.*`, `ChartRecordActions.*`, `CourseRecordActions.*`,
  `RecordFileActions.*`, and `RecordsIrActions.*` share Records behavior between
  Main Menu and Music Select.
- `replay/ReplayExportJob.*` owns export execution and queued progress/results;
  `scene/ReplayRecordTask.*` owns cancellable preparation and UI completion.
- `ReplayVideoExporter.*` and `ResultImageExporter.*` produce user artifacts.

## Boundaries and invariants

Replay capture uses the immutable attempt provenance and records chart-time
input. Result rows, replay files, and IR snapshots are separate durable facts
linked by verified references; one must not materialize or mutate another as a
side effect. Playback/export works from a prepared chart agreement and fails
closed when identity, codec, or file checks do not agree.

Both selectors expose chart and saved-course Records, including result recall,
BRD sharing/deletion, and available IR actions. Recall returns to the originating
selector. Preparation continues while the app is backgrounded, with navigation
held until resume. Audio loading failure alone does not reject replay Watch or
G-Battle; cancellation still prevents launch. Autoplay Watch reuses a matching
preloaded chart's RANDOM choices, otherwise generating fresh ones. Autoplay
Watch and G-Battle honor the selected ClubMode; autoplay Watch hides touch
visualization and replay ghosts.

Exports hold their reservation until the UI consumes the terminal result and
joins the worker. Preview media must be released before exporting. Sharing
retains a verified file snapshot until the asynchronous document operation ends.

## Verification

Start with `beatoraja_replay_codec_tests`, `replay_*_tests`,
`replay_repository_modern_chart_tests`,
`replay_repository_modern_course_tests`,
`result_image_exporter_partial_tests`, and `replay_playfield_presentation_tests`.
Consult the
[file-replay contract matrix](../replay/file-replay-contract-matrix.md) for
format and lifecycle coverage.

## Related pages

- [Courses](courses.md)
- [Results, records, and persistence](results-records-and-persistence.md)
- [Practice and analysis](practice-and-analysis.md)
