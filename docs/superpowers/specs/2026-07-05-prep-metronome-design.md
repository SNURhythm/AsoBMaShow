# Prep Metronome Design

## Goal

Add an optional one-measure count-in before chart playback starts. The count-in uses generated metronome clicks at the chart's effective tempo and guessed time signature, so players can prepare before the first playable chart event.

## Scope

The setting applies to normal gameplay, autoplay, replay playback, and course playback when enabled. Chart preview playback remains unchanged. Existing chart note timings, replay timestamps, score calculations, and chart database metadata stay compatible with existing saves.

## Parser Metadata

BMS has no time signature metadata, so the parser will expose a lightweight guess on `ChartMeta` while it is already walking measures and timelines:

- `MostPrevalentBpm`: the BPM value with the longest accumulated chart duration.
- `GuessedBeatsPerMeasure`: the most common whole-number beat count inferred from measure scale.

The parser will accumulate BPM durations during its existing timing pass instead of doing a second pass. It already knows `currentBpm`, each measure's `Scale`, timeline positions, stops, and elapsed duration at that point.

The time-signature guess is intentionally narrow:

- `Scale` near `1.0` means 4 beats.
- `Scale` near `0.75` means 3 beats.
- `Scale` near `0.5` means 2 beats.
- Other finite positive scales map to `round(scale * 4)` and clamp to a practical range of 1 to 16.
- The final chart guess is the beat count with the most total chart duration, falling back to 4 when there is no usable evidence.

The prep BPM uses `Meta.Bpm` when it is finite and sane. If the metadata BPM is invalid or outside the sane range of 30 to 300 BPM, playback uses `MostPrevalentBpm` when that is valid. If both are unusable, the fallback is 120 BPM.

Parser changes must be made in `../bms-parser-cpp`, tested there with `make clean && make test && make test_amalgamation`, then copied from `../bms-parser-cpp/build/bms_parser.hpp` and `../bms-parser-cpp/build/bms_parser.cpp` into this repo's `src/`.

## Runtime Architecture

The app will not physically insert or shift chart timelines. Instead, gameplay starts the song clock at a negative time equal to one synthetic measure:

```text
-prepMeasureDuration ... -beat ... 0 ... original chart timing
```

This keeps all original chart times stable. Replays still find notes by their saved `noteTimeMicros`, replay events still fire at saved `songTimeMicros`, and recorded replays remain comparable with old data.

`GamePlayScene` will decide whether prep is active from `AppSettings` and playback context. When active, it will compute:

- prep BPM from `ChartMeta.Bpm` or `ChartMeta.MostPrevalentBpm`;
- beat count from `ChartMeta.GuessedBeatsPerMeasure`;
- beat interval in microseconds;
- total lead-in duration as `beatInterval * beatCount`.

It will start `Jukebox` at `-leadInMicros` and schedule metronome clicks at negative beat times. The original chart audio list remains unchanged, so chart audio begins at time 0 as before.

## Audio Behavior

Metronome sounds are generated internally, not loaded from chart assets. This avoids adding binary assets and avoids depending on chart WAV tables.

There will be two built-in clicks:

- accent click for the first beat of the prep measure;
- regular click for every remaining beat.

The audio layer will support scheduling these generated sounds alongside chart audio. The first click plays at `-leadInMicros`; the last click plays one beat before chart time 0.

## Settings

Add a persisted boolean setting, default off:

```text
prepMetronomeEnabled=false
```

Expose it in the settings UI near gameplay/audio timing controls as `Prep Metronome`, with summary text `Enabled` or `Disabled`.

## Data Flow

1. Parser fills `ChartMeta.MostPrevalentBpm` and `ChartMeta.GuessedBeatsPerMeasure`.
2. Gameplay loads and schedules the original chart normally.
3. If prep is enabled and the scene is not chart preview, `GamePlayScene` computes a `PrepMetronomePlan`.
4. `Jukebox` starts playback from the plan's negative start time.
5. `Jukebox` schedules generated prep clicks at the plan's negative beat times.
6. Gameplay update, rendering, judgement, replay, and chart audio continue to use original chart timestamps.

## Edge Cases

- Invalid or extreme metadata BPM uses the most prevalent BPM.
- If the chart has no usable BPM evidence, prep uses 120 BPM.
- If the guessed beat count is invalid, prep uses 4 beats.
- Practice lead-in remains separate. If both practice lead-in and prep metronome are active, practice seek behavior must continue to honor practice start position while prep is only applied before actual playback start for that scene.
- Seeking and retry must reset generated prep click state just like chart audio state.
- Chart preview must not enable prep even if the setting is on.

## Testing

Parser tests in `../bms-parser-cpp` will cover:

- most-prevalent BPM computed during the timing pass;
- insane metadata BPM falling back to prevalent BPM through helper logic;
- guessed beat count for common scales: 1.0, 0.75, and 0.5;
- default fallback to 4 beats.

App-side tests or focused compile-time checks will cover:

- prep plan generation from chart metadata;
- disabled setting producing no prep plan;
- preview context excluded from prep;
- negative start time and click schedule for a 4/4 120 BPM chart.

Final verification will include:

- `make clean && make test && make test_amalgamation` in `../bms-parser-cpp`;
- copying the amalgamated parser files into `src/`;
- `cmake --build cmake-build-debug --target main -j 6` in this repo.
