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
- `ReplayVideoExporter.*` and `ResultImageExporter.*` produce user artifacts.

## Boundaries and invariants

Replay capture uses the immutable attempt provenance and records chart-time
input. Result rows, replay files, and IR snapshots are separate durable facts
linked by verified references; one must not materialize or mutate another as a
side effect. Playback/export works from a prepared chart agreement and fails
closed when identity, codec, or file checks do not agree.

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
