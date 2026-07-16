# Start Lane Indicators Design

## Goal

Add an opt-out, enabled-by-default "ここからスタート" preparation cue that
shows downward triangles over the lanes used by the first playable-note
timeline. Normal gameplay gets a two-second silent cue before its optional
preparation metronome. Practice reuses its existing count-in without adding
time.

The cue must behave consistently in live play, retries, autoplay, course play,
interactive replay, course replay, and offline replay video export.

## Settings and Scope

Add a persisted `startLaneIndicatorsEnabled` boolean to `AppSettings`. Its
default is `true`, including when an older JSON or legacy settings document
does not contain the field. Expose it on the Visual settings tab as **Start
Lane Indicators**, with `Shown` and `Hidden` states.

The indicator setting is independent of `prepMetronomeEnabled`:

- indicator on, metronome on: two-second silent cue, metronome count-in, chart;
- indicator on, metronome off: two-second silent cue, chart;
- indicator off: retain the existing metronome and chart timing;
- practice: retain the existing count-in duration and show the cue throughout
  that count-in when the indicator setting is on.

Chart preview playback remains unchanged. Offline replay video export follows
normal replay playback: it includes the silent cue, the preparation metronome
when enabled, and then the chart.

## Cue Lane Selection

Selection runs against the chart after play options and lane randomization
have been applied.

For normal starts, find the earliest timeline containing at least one playable
visible note. For practice starts and loop restarts, find the earliest such
timeline at or after the configured practice start and before the practice
range ends. Every playable lane on that one timeline receives a marker.

Playable notes for this cue are normal notes and long-note heads. Ignore:

- invisible notes;
- landmines;
- long-note tails; and
- null or malformed measures, timelines, and notes.

If an active range contains no qualifying timeline, draw no indicators. Normal
play adds no two-second delay in that case; practice retains its existing
count-in unchanged.

## Timing Architecture

Introduce a preparation cue plan with the selected lanes, playback start time,
and indicator end time. `GamePlayScene` owns this timing plan and tells the
renderer whether the cue is currently visible. The renderer owns only lane
geometry and drawing.

Do not shift or insert chart timelines. Extend the existing negative-time
preparation model:

```text
indicator on, metronome on:
  cue start ---- 2 seconds ---- first metronome click ---- count-in ---- chart

indicator on, metronome off:
  cue start ---- 2 seconds ---- chart

practice:
  existing count-in start ---- cue visible ---- practice start
```

For normal play, the indicator ends when the first preparation-metronome click
begins, or at the chart/audio seek anchor when the metronome is disabled. The
Jukebox starts two seconds before that boundary. For practice, the indicator
starts with the existing count-in plan and ends at the configured practice
start; no extra time is added.

Retries and course stage transitions rebuild the plan. Interactive replay and
course replay use the same plan construction as live play. Offline replay
export derives the same negative start, indicator boundary, metronome clicks,
and output duration so its opening matches interactive replay.

## Preparation Input Behavior

While the indicator is visible, chart judgement is held. Lane input remains
available as preparation feedback but cannot hit or miss notes:

- presses and releases animate lane beams;
- touches appear in the touch visualizer;
- the floating lane cover can be moved; and
- these interactions are recorded and reproduced by replay.

Preparation lane presses and releases are stored as replay events with no
judgement and their signed chart timestamps. Touch samples already support
signed timestamps and retain them. Lane-cover events must also retain signed
timestamps instead of clamping them to zero.

The initial lane-cover replay snapshot is timestamped at the preparation
playback start, not the later chart or practice start. Subsequent preparation
adjustments therefore append in chronological order. Interactive replay,
course replay, and export process those negative events before chart time zero.
Older replays without preparation events remain valid.

## Rendering

Draw one filled downward triangle centered in each selected lane:

- white-key lanes use white;
- blue-key lanes use blue; and
- scratch lanes use red.

Use the renderer's post-randomization lane order and existing symmetric
white/blue/scratch classification, overriding the scratch marker color to red.
Add a small triangle primitive to `SimpleBatchRenderer`; no texture, text, or
animation is required.

Triangles render after notes but before the lane cover. Their bases sit below
the lane-cover edge with a small constant gap while sufficient visible lane
space remains. As the floating cover moves downward, the triangles follow its
edge until doing so would leave insufficient space above the judge line. At
that point the triangles stop moving, enter the cover region, and are hidden
by the lane cover's later draw pass. The gap must remain visible in all
non-occluded positions.

The same geometry and depth ordering apply to live play, interactive replay,
course replay, and offline replay export.

## Data Flow

1. Gameplay or export builds the existing metronome/count-in plan.
2. Cue planning finds the first qualifying timeline in the active range.
3. Normal play extends the playback start by exactly `2,000,000` microseconds;
   practice reuses the count-in start.
4. The scene/export clock determines whether the indicator is visible.
5. `BMSRenderer` draws the selected lane triangles at a depth below the lane
   cover.
6. Live preparation input records signed no-judgement lane events, touch
   samples, and lane-cover events.
7. Replay playback and export consume those events on the same signed clock.

## Verification

Focused tests must cover:

- first-timeline lane selection for chords and post-randomization charts;
- normal notes, long-note heads, long-note tails, invisible notes, landmines,
  null entries, empty charts, and practice range boundaries;
- the exact two-second normal, replay, course-replay, and export extension with
  the metronome both enabled and disabled;
- unchanged practice count-in duration and cue visibility through the entire
  count-in;
- disabled indicators preserving existing timing;
- enabled-by-default loading, settings JSON round trips, and legacy parsing;
- white, blue, and red lane classification;
- triangle placement, the lane-cover gap, and lane-cover occlusion when space
  runs out;
- chronological signed lane, touch, and lane-cover replay events; and
- offline export start time, metronome scheduling, frame duration, and cue
  visibility.

Final verification runs the focused test targets, the relevant replay/export
tests, and:

```bash
cmake --build cmake-build-debug --target main -j 6
```

## Out of Scope

- Rendering the Japanese `ここからスタート` text.
- Animating or texturing the triangles.
- Changing note, judgement, score, gauge, or chart timestamps.
- Adding preparation behavior to chart preview playback.
- Changing preparation metronome tempo or beat selection.
