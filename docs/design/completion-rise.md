# Native completion celebration

The built-in renderer implements the Rise direction from
[`tools/completion-motion`](../../tools/completion-motion/README.md).
The authoring project remains useful for visual iteration; no JavaScript,
Remotion, or new effect runtime is shipped in the game.

## Event and result policy

`completion_celebration::Tracker` observes committed judgement events. Stage-local
PGREAT/GREAT/GOOD/BAD/POOR counts must cover the chart's effective scorable
total. The shared `clear_policy::completedClearRank` determines the class:
GOOD permits Full Combo; only PGREAT/GREAT earns Perfect. An all-PGREAT MAX
uses the Perfect animation. Beatoraja also requires the surviving combo to
cover the stage, preserving its empty-POOR rules.

Classic LN heads supply input-timing samples only, so the effect waits for the
tail's scoring judgement. CN/HCN count both scoring endpoints. This is immediate
feedback at the final resolved judgement, not a new terminal event or a promise
about the persisted result. Later empty POOR does not replay or downgrade the
animation; scoring and final-clear persistence retain their existing rules.

The realtime worker retains the event independently of its bounded visual
transaction history. A presentation stall can lose older visual judgements
without losing the achievement or its timestamp. Replay/export and legacy
gameplay use the same reducer in `PlayfieldVisualStateStore`.

The event latches once until the attempt's worker/store is reset (retry,
seek/restart, new course stage) or destroyed. Course stages count their own judgements even
when the displayed combo is carried. Practice and settings previews are
excluded. Assist-marked and altered-playback attempts are excluded through the
existing assist policy; autoplay and replay can demonstrate the animation.
Active failure presentation suppresses it.

The start is the final judgement's `visualTimeMicros`, including the same visual
offset as other judgement effects. Captured frames retain the event. Live play,
replay watch, and video export evaluate age from their visual frame clock;
wall time, animation counters, and frame rate do not advance it.

Only the built-in render pass draws it. The selected-skin HUD pass does not;
custom skins retain their authored timers and animations.

## Motion and rendering

- The independent title anchor is `(640, 285)` in a 1280×720 design space,
  centered within the safe area. Uniform scale fits both available dimensions,
  including portrait screens. Judgement placement settings do not move it.
- Full Combo retains the seven rising trails, ten drifting shards, staggered
  Noto Sans CJK JP bold letters, and a short lime underline. Its 80 ms entrance
  delay is followed by a 230 ms cubic ease-out rise and a readable hold;
  exit fades from 2.05 to 2.45 seconds.
- Perfect has one synchronized onset at 80 ms: lettering, four-facet mark,
  fourteen side shards, and a single gold elliptical pulse centered on the
  visible lettering. The mark stays 88 design units above the word. The pulse
  decays without a second emission. Exit fades from 2.35 to 2.75 seconds.
- `CompletionCelebrationEffect.h` evaluates bounded geometry directly. It emits
  at most 576 colored vertices plus seven or nine textured glyph quads. There
  are no particle containers, blur passes, shaders to compile, or audio cues.
- The renderer pre-rasterizes the sixteen visible glyph slots at construction.
  Frames reuse those textures and the existing batch buffers; motion changes
  quad geometry and opacity, not font rasterization or text layout.
- The authoring review's reduced-motion switch remains a prototype option.
  Native gameplay has no corresponding motion preference yet.

## Verification

Focused coverage lives in `playfield_visual_state_tests`,
`realtime_gameplay_worker_tests` (including transaction-history overflow),
`replay_playfield_presentation_tests`, and
`builtin_renderer_characterization_tests`. The latter draws through the actual
Metal renderer and checks distinct variants, progression, pre-event absence,
and completed exit against a fixed synthetic playfield.

For optional local inspection, set `ASOBMASHOW_BUILTIN_ARTIFACT_DIR` to write
representative PNGs. Also setting `ASOBMASHOW_COMPLETION_ANIMATION_DIR` writes
30 fps PNG sequences under `full-combo/` and `perfect/`. These opt-in captures
do not slow normal test runs and are not successful-play recordings.

Desktop captures do not establish device GPU performance; iOS and Android
device appearance/performance still need validation.
