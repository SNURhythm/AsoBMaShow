# Practice and Timing Analysis Design

## Goal

Turn Aso's chart-cursor practice into a complete section-practice and timing
analysis system. Players can define a section, repeat it with a beat-aligned
count-in, restart it immediately, change practice rules, save chart-specific
presets, and use recorded timing data to choose the next section to practice.

Playback rate is also a normal single-chart play option. Any non-100% rate is
an assist and caps the clear mark at Assisted Easy. The initial implementation
changes pitch with rate. Pitch-preserving time stretch is a later audio backend
that users will be able to select through the same playback-mode interface.

Internet ranking is out of scope.

## Reference behavior and existing foundations

`ChartViewerScene.cpp` currently stores one chart cursor and starts practice at
that time with a fixed three-second lead-in. Gameplay then continues through
the remainder of the chart. `GamePlayScene::reset()` already resets notes,
gauge state, replay capture, and audio position without reparsing the chart,
and the logical Retry action already reaches the current-pattern restart path.
Those paths are extended instead of creating a separate gameplay engine.

`ReplayData.h` already records lane, note time, judge time, judgement, signed
timing delta, gauge, combo, and score for press, release, miss, and mine events.
The analytics system consumes this existing evidence. New provenance fields
describe playback and judge overrides; individual timing events do not need a
second persistence format.

Beatoraja's `PracticeConfiguration.java` provides start/end positions,
frequency, initial gauge, judge rank, and practice graphs, and stores settings
per chart hash. Aso follows the content-addressed per-chart model but keeps its
stronger replay-backed analytics and integrates practice with results and
saved replays.

## Chosen architecture

Use a shared practice domain model and focused services while retaining the
existing scenes as entry points:

- Chart Viewer edits ranges and configuration.
- Gameplay executes an immutable practice session configuration.
- Result and replay views consume one analytics model and create new practice
  ranges.
- A profile-scoped store owns last-used and named per-chart presets.
- The audio layer owns playback-rate processing behind a mode-independent
  interface.

Putting all behavior directly in the three scenes was rejected because state
would be copied across retries, results, and replay flows. A new dedicated
Practice Scene was also rejected for the first implementation because it would
duplicate Chart Viewer's parsing, rendering, scrolling, and marker behavior.
The shared model does not prevent a dedicated scene later.

## Practice configuration

`PracticeConfiguration` is the single serializable source of truth. It
contains:

- chart SHA-256;
- start and end positions in the original chart timeline;
- loop enabled state;
- count-in length in beats;
- gauge type and optional starting gauge percentage;
- judge-window override;
- playback rate and playback mode; and
- optional preset identity and display name.

The judge override is a tagged representation. Version 1 stores a percentage
scale applied uniformly to the effective chart judge windows. The type and
serialization reserve a custom-windows variant so later per-judgement early
and late windows do not require replacing the practice model. A scale of 100%
means no override.

Playback rate is represented as an exact integer percentage together with a
mode enum. `PitchShift` is supported initially. `TimeStretch` is a recognized
but unavailable mode until its DSP processor is implemented. Unsupported
modes must never silently start using different audio behavior.

User-facing configuration ranges are deliberately bounded:

- count-in: 0 through 16 whole beats, default 4;
- playback rate: 50% through 200% in 5% steps, default 100%;
- judge-window scale: 25% through 200% in 5% steps, default 100%; and
- starting gauge: 0% through 100% in 1% steps when enabled.

For a chart with no saved practice data, the cursor becomes the start marker,
the chart end is the end marker, looping is off, and other options use their
neutral/current play-setting values. This preserves the current "play the
remainder" workflow while making section looping one toggle away.

Start and end positions always use unscaled chart microseconds. Configuration,
presets, heatmap sections, and replay-derived ranges therefore remain stable
when playback rate changes. Validation clamps both markers to the playable
chart range and requires a non-empty ordered section. The UI automatically
orders or clamps crossed markers instead of storing an invalid range.

## Practice session lifecycle

`PracticeSession` owns an immutable configuration snapshot and mutable attempt
state:

- entry source: Chart Viewer, result, or saved replay;
- current loop number;
- current partial attempt;
- completed loop attempts and their replay events; and
- aggregate analytics built from completed attempts.

Starting practice loads chart resources once and creates the session. Every
iteration performs the configured count-in, plays from the start marker, and
finishes when original chart time reaches the end marker. With looping enabled,
the completed attempt is retained and the next iteration begins its own
count-in without opening Result Scene. With looping disabled, reaching the end
marker opens the practice summary after the single completed attempt.

Instant restart abandons the current partial attempt, resets gameplay state,
and begins that iteration's count-in. It must reuse the parsed chart and loaded
audio resources. Abandoned attempts may appear in detailed session history as
incomplete but are excluded from headline statistics. They cannot save a score
or be confused with completed analytics attempts.

Finishing practice opens one summary containing all completed loops. Exiting
without a summary discards the in-memory session. Practice remains
score-ineligible and does not persist ordinary score records. An explicitly
saved replay may preserve a chosen attempt through the normal replay database.

## Chart Viewer experience

The chart canvas displays distinct start and end marker bars and shades the
selected range across chart columns. `Set Start` and `Set End` select the marker
that the next chart tap moves. Marker controls remain usable with touch, mouse,
and controller/keyboard navigation.

A compact Practice panel exposes:

- section start and end;
- loop toggle;
- count-in beats;
- gauge type and starting amount;
- judge-window percentage;
- playback rate and mode;
- last-used or named preset selection;
- Save As, update, rename, and delete for named presets; and
- Start Practice.

The existing toolbar Practice action remains a fast path and starts with the
last-used valid configuration. Editing any field updates the last-used
configuration automatically. Saving a named preset creates an independent
snapshot; later last-used edits do not mutate it unless the user explicitly
updates that preset.

When analytics opens Chart Viewer through `Practice This Section`, the
suggested start/end range is installed while the chart's other last-used
settings remain intact. The Practice panel is shown before playback so the
user can adjust the suggestion.

## Gameplay behavior and overrides

The practice HUD shows loop number, selected range, playback rate, and
count-in state. The existing mapped Retry command and a persistent touch
Restart control invoke instant restart. The pause menu offers Resume, Restart
Section, Finish Practice, and Exit Without Summary.

Count-in is expressed in beats and runs before every loop iteration, including
after instant restart. It is aligned to the tempo effective at the start
marker and follows playback rate. The count-in does not change stored section
positions or introduce playable notes before the start marker.

Gauge setup first performs the normal gauge configuration, then applies the
optional starting percentage through the existing gauge snapshot/restore
mechanism. The percentage is clamped to the selected gauge's valid range.

Judge setup first resolves chart rank and course constraints as it does today,
then scales the resulting concrete early and late windows. The exact effective
windows and override source are captured at play start. This keeps replay
judgement deterministic and lets a future custom-windows variant enter at the
same boundary.

Crossing the end marker finalizes the current practice replay before resetting
state. Notes outside the configured range are skipped and do not contribute to
score, gauge, combo, misses, or analytics.

## Playback rate in practice and normal play

`PlaybackRate` travels through play-start options, retries, result actions,
practice sessions, replay recording, and replay playback. A single transformed
gameplay clock drives audio, chart notes, BGA, seeking, and section boundaries.
The rate processor must not allow independently scaled clocks to drift.

Runtime song, note, judge, and section positions remain in original-chart
microseconds and advance at the selected rate relative to monotonic real time.
Recorded event deltas therefore remain in that deterministic chart-time
domain. Base judge windows are multiplied by playback rate and then by the
user's judge scale so their real-time duration stays consistent. Analytics
divides recorded chart-time deltas by the recorded rate and exposes the real
milliseconds the player experienced.

Pitch-shifting mode consumes decoded audio at the selected ratio and changes
pitch with speed. Resource decoding remains reusable across restarts and loops.
The later pitch-preserving processor implements the same clock and seek
contract, so adding it does not change practice configuration, presets,
provenance, or UI data flow.

Normal single-chart play exposes playback rate and mode with the other play
options. At 100%, eligibility and clear behavior remain unchanged. Any other
rate:

- marks score provenance as modified;
- applies the same assisted-clear behavior as existing assist options;
- caps the achieved clear mark at Assisted Easy;
- remains recordable under existing assisted-play rules; and
- displays the clear cap before play starts.

Replay playback uses the recorded rate and mode automatically. A replay whose
processor is unavailable is rejected with a clear status rather than played at
the wrong speed. Old scores and replays default to 100%, PitchShift, and no
judge scale.

## Timing analytics

`PracticeAnalytics` is a pure module that accepts chart structure plus one or
more `ReplayData` attempts. Result Scene, practice summary, saved replay views,
and tests all use the same output.

Press and release judgements with meaningful timing deltas become timing
samples. Long-note heads and tails remain distinct actions. Misses are counted
and located but excluded from mean, standard deviation, median, and timing
histograms because they do not represent a bounded hit delta. Mines and
unjudged input events are excluded.

Timing values are expressed in signed real milliseconds experienced by the
player: negative is early and positive is late. Playback rate, playback mode,
judge scale, and effective windows remain attached to each attempt's
provenance. The UI must not silently aggregate attempts with incompatible
timing conditions.

Analytics outputs:

- signed arithmetic mean;
- standard deviation and median;
- sample, early, late, and miss counts;
- a histogram using 5 ms bins plus overflow buckets;
- per-lane mean, deviation, sample count, early/late balance, and misses; and
- measure-based section timing bias, consistency, and miss/bad rate.

The UI shows aggregate statistics and lets the player inspect individual loop
attempts. The section heatmap may combine adjacent measures visually to fit
the available width, but it preserves exact measure boundaries in its data.
Tapping selects a heatmap section; dragging selects adjacent sections. The
selected original-chart range feeds `Practice This Section`.

## Result and replay entry points

Practice results add an analytics panel beside the existing result content.
The panel switches between histogram, per-lane bias, and heatmap views and
between aggregate and individual attempts. It owns the selected analytics
range and exposes `Practice This Section` when a valid range exists.

Normal results use the just-recorded replay events for the same analytics and
action. Saved replay detail/viewing loads its persisted replay and chart, then
uses the same module. Auto-play replays may display analytics but cannot create
misleading player timing statistics; their samples are labeled Auto and are
not presented as player bias.

The action routes through one practice-launch request rather than constructing
scene-specific start options. The request contains chart identity, suggested
range, entry source, and optional replay reference. Chart Viewer resolves the
chart, applies the range to the last-used practice configuration, and opens the
Practice panel.

## Persistence and portability

Per-chart practice data is stored lazily at
`profiles/<profile-id>/practice/<sha256>.json`. Each file contains:

- schema version;
- matching chart SHA-256;
- last-used configuration; and
- zero or more named preset records with stable IDs and display names.

Files use strict bounded parsing and atomic replacement. A malformed optional
practice file falls back to defaults and reports a concise status without
blocking profile activation or chart viewing. A preset for a different hash
never attaches to the selected chart.

`PlayerProfilePaths` and profile duplication, export, import, deletion, and
validation include the practice directory. Import validation permits only
regular, hash-named JSON files within that directory and rejects path escape or
unsafe links. Normal-play playback rate and mode remain ordinary active-profile
settings because they are not chart-specific.

Playback rate, mode, judge override, and effective windows are serialized in
score/replay provenance. The provenance schema remains backward compatible:
missing fields receive the neutral defaults. Practice preset schema migration
is independent from score and replay database schema migration.

## Error handling

- Invalid or empty ranges disable Start Practice and explain the range issue.
- Unsupported playback modes cannot silently fall back during play or replay.
- Failure to save a last-used preset does not destroy the previous atomic file;
  practice may continue with an in-memory configuration and visible warning.
- Playback-rate initialization failure aborts scene start before score or
  replay recording begins.
- Loop finalization is ordered before reset so a completed attempt is retained
  exactly once.
- Restart and scene exit finalize or abandon the partial attempt explicitly.
- Analytics with no valid samples shows counts and misses without NaN or
  fabricated zero-valued statistics.
- Old replay data remains readable and analyzable with neutral playback
  defaults.

## Delivery and commit boundaries

Implementation is delivered as independently reviewable feature commits, not
one cross-cutting final commit:

1. Practice configuration, validation, and per-chart preset storage.
2. Score/replay playback provenance and backward-compatible serialization.
3. Chart Viewer start/end markers and Practice panel.
4. Gameplay section end, looping, count-in, and instant restart.
5. Starting gauge and scalable judge-window overrides.
6. Pitch-shifting playback rate and authoritative transformed clock.
7. Normal-play rate controls and Assisted Easy clear cap.
8. Pure replay timing analytics.
9. Practice result analytics and multi-attempt summary.
10. Result/replay `Practice This Section` entry points.
11. Profile portability and final integration coverage where it cannot be
    included safely with the owning storage commit.

Tests normally land with the feature they protect. Mechanical build-system or
schema migration changes may be separate commits when that makes rollback and
review clearer.

## Verification

Focused tests must cover:

- range clamping, ordering, serialization, and chart-hash isolation;
- last-used and named preset round trips plus atomic-save failure;
- profile duplicate/export/import behavior for practice files;
- end-marker completion, per-loop count-in, resource-reusing restart, and
  incomplete-attempt exclusion;
- initial gauge application and scaled effective judge windows;
- rate-clock audio/chart synchronization, seeking, looping, retry, and replay;
- 100% neutral eligibility and non-100% Assisted Easy clear capping;
- old and new provenance/replay compatibility;
- histogram bins, signed statistics, misses, long-note actions, lane bias,
  heatmap boundaries, and incompatible-attempt grouping; and
- range handoff from normal result, practice result, and saved replay.

The integration gate is all application CTest entries with Yoga excluded, a
desktop `main` build, and relevant unsigned/build-only mobile compile checks.
Manual acceptance covers marker manipulation and analytics selection on touch
devices, audible rate changes, repeated loop/restart synchronization, and
normal-play assisted-clear labeling on desktop, iPad/iPhone, and Android.
