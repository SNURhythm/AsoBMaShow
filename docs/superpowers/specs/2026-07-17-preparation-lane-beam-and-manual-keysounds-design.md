# Preparation Lane Beam and Manual Keysounds

## Goal

Normal gameplay input during the two-second start-lane indicator must produce
lane-beam feedback and manual keysounds without judging notes, changing gauge,
or advancing automatic gameplay deadlines.

Manual keysound selection must also work outside judgement windows during the
rest of an attempt. This restores the traditional beatmania behavior where a
player can perform with the chart's lane keysounds during empty sections.

## Selection rule

Keysound selection is independent from whether a press can judge a note, but a
judgeable note has first priority:

1. If the normal manual note picker finds a judgeable press candidate, use
   that candidate's keysound.
2. Otherwise, use the first pressable note at or after the compensated input
   time across the main and compensation lanes.
3. If no future pressable note exists on either candidate lane, use the
   chronologically last pressable note across those lanes.
4. If neither candidate lane contains a pressable note, do not submit a
   keysound.

Normal notes and long-note heads are pressable keysound sources. Long-note
tails and landmines are excluded. Practice attempts restrict candidates to the
session's half-open note range `[startMicros, endMicros)`.

The future and last-note fallback searches are timeline-based. They do not
filter on runtime note state: played or dead notes remain candidates. In
particular, after every note on a lane has passed, the last note continues to
supply its keysound even if that note was already pressed and is dead.

For equal-time candidates, the main lane wins over the compensation lane.
Within a lane, stable chart order breaks any remaining tie. A selected note
whose WAV is `NoWav` remains silent; selection does not skip it to find a later
audible note.

Submission still respects the existing attempt policy: manual keysounds are
not duplicated when automatic keysounds are enabled, and replay playback does
not synthesize live manual keysounds. This design changes candidate selection,
not those ownership rules.

## Shared selection policy

Realtime `GameplaySimulation` and legacy `RhythmLaneInputController` use one
pure keysound-selection policy over ordered per-lane note views. Each caller
adapts its existing note identity (`NoteId` or `bms_parser::Note *`) without
changing the rule.

Candidate lane timelines are ordered or indexed during chart/controller
construction. A press performs bounded lane lookups and binary searches only;
it does not allocate or scan the full chart. This keeps the high-frequency
realtime path deterministic.

## Realtime preparation transactions

The realtime worker keeps the existing activation time for normal gameplay.
Before that time, automatic advancement remains blocked. Input is no longer
dropped, however:

- a press selects and reserves its manual keysound;
- the simulation records a visual-only press and updates lane-held state;
- the worker records the ordered lane-visual transaction;
- the reserved audio command is committed;
- a release records the matching visual-only release and clears lane-held
  state.

These preparation transactions may add replay press/release events, matching
the existing legacy preparation path, but they never resolve a note, emit a
judgement, change score/combo/gauge, or advance miss/autoplay deadlines.

The existing fail-closed audio ordering remains: a required sound reservation
must succeed before lane state is committed. An audio capacity or commit
failure invalidates the attempt through the existing realtime fault path.

A key held across the activation boundary stays visually held but does not
automatically hit a note. The player must release it and press again; the
post-activation press then follows ordinary judgement and keysound rules.

Session-backed practice continues to omit the normal activation gate, so its
approved count-in judgement behavior is unchanged.

## Legacy path

The legacy lane controller applies the same selection rule after its existing
manual judgement candidate search. An empty or unjudgeable press still changes
the lane beam and replay state, and now also returns the selected fallback
keysound for immediate submission by `GamePlayScene`.

The existing normal-play preparation branch is updated to use this shared
visual-plus-keysound rule instead of recording a silent visual event directly.
It remains judgement-free.

## Verification

Regression coverage will establish:

- a judgeable note's keysound wins over fallback selection;
- a press before the first note selects the first note;
- a press between notes selects the next note;
- a press after all notes selects the last note even when it is played/dead;
- long-note tails and landmines are not press keysound sources;
- practice fallback selection cannot escape its half-open range;
- main-lane equal-time candidates beat compensation-lane candidates;
- realtime preparation press/release publishes ordered lane visuals and a
  keysound without note, judgement, gauge, or deadline mutation;
- a key held through activation cannot auto-hit;
- realtime and legacy paths return the same keysound identity for equivalent
  charts and input times;
- existing rapid-input, practice count-in, autoplay, replay, and audio fault
  regressions continue to pass.

Desktop compilation, the full focused CTest set, and a non-deploying iOS build
check will verify integration.
