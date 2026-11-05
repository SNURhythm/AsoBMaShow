# Native completion celebration

The built-in renderer implements Rise, calm FINISH, and MAX Focus from
[`tools/completion-motion`](../../tools/completion-motion/README.md).
The authoring project remains useful for visual iteration; no JavaScript,
Remotion, or new effect runtime is shipped in the game.

## Event and result policy

`completion_celebration::Tracker` observes committed judgement events. Stage-local
PGREAT/GREAT/GOOD/BAD/POOR counts must cover the chart's effective scorable
total. The shared `clear_policy::completedClearRank` determines the class:
GOOD permits Full Combo; PGREAT/GREAT earns Perfect. All-PGREAT completion
earns the distinct MAX Focus animation through the same MAX clear rank. Beatoraja
also requires the surviving combo to cover the stage, preserving its empty-POOR rules.

Below those achievements, resolved-note progress latches a neutral `FINISH`.
It waits for actual resolution rather than counting nonvanishing BAD samples;
a final suppressed PMS miss still supplies its event timestamp. FINISH also waits
until the latest authored tail of an active playable long note, even when a miss
or early release resolved its score sooner. The presentation store caches this
tail boundary per chart and maps it into the frame's visual clock. Its start is
the later of final resolution and that tail, so no later judgement is needed
and the full animation still plays. The same rule covers worker snapshots,
replays/exports, and assisted achievements shown as FINISH. Earned MAX, Perfect,
and Full Combo retain their judgement timing. Incomplete or
aborted attempts cannot reach that progress threshold. FINISH is not a clear
lamp and does not alter stored results.

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
excluded. Assist-marked and altered-playback whole-chart attempts receive only
FINISH through the existing assist policy; autoplay and replay can demonstrate
the applicable animation.
Active failure presentation suppresses it.

The start is the final resolution's `visualTimeMicros`, including the same visual
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
- MAX implements the approved Focus direction in `max-frame.mjs`. Short strokes
  gather over 140 ms; 140 px lettering enters over 200 ms and settles from 1.24×
  scale over 320 ms. An eight-point white/gold/teal star sits 118 design units
  above the word. Twenty-four shards and two teal/gold rings share the 140 ms
  burst onset, then clear while the title and star hold. Exit fades from 2.55 to
  3.05 seconds. The local scrim and two glows match the approved prototype.
- FINISH uses muted 62 px lettering with a 6 px lift over 320 ms, a hold until
  1.45 seconds, and a 450 ms exit. A soft dark backdrop and faint neutral halo
  support readability. Its stationary low lane glow fades within 800 ms; it has
  no travelling Wave, pulse, mark, shards, or underline.
- `CompletionCelebrationEffect.h` evaluates bounded geometry directly. It emits
  at most 2192 colored vertices plus three, six, seven, or nine textured glyph quads. There
  are no particle containers, blur passes, shaders to compile, or audio cues.
- The renderer pre-rasterizes the twenty-five visible glyph slots at construction.
  Frames reuse those textures and the existing batch buffers; motion changes
  quad geometry and opacity, not font rasterization or text layout.
- A local black scrim sits behind the native celebration, reaching 68% opacity
  in 160 ms and fading with the lettering. Its solid elliptical center feathers
  to transparent edges; Perfect's backdrop also covers the mark. This readability
  refinement is native-only; the original authoring studies retain their backdrop.
- A faint lime/gold radial glow above the scrim follows the lettering's fade;
  Perfect also has a smaller gold halo behind its mark. Neither glow pulses.
- The selected soft Wave fills the lane surface beneath notes and opaque covers.
  It shares the title's captured event/clock, lasts 550 ms for Full Combo or
  650 ms for Perfect, and uses a borderless asymmetric gradient with a broad wake.
  `CompletionWaveEffect.h` clips 24 gradient strips to the lane bounds (at most
  96 additional vertices); the gameplay camera supplies perspective. No blur
  pass, texture, screen-space overlay, or new completion event is needed.
  MAX uses the same clipped 24-strip path for its stronger white/teal crest and
  soft wake: starts at 140 ms, travels over 800 ms, and clears by 950 ms.
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
30 fps PNG sequences under `finish/`, `full-combo/`, `perfect/`, and `max/`. These opt-in captures
do not slow normal test runs and are not successful-play recordings.

Desktop captures do not establish device GPU performance; iOS and Android
device appearance/performance still need validation.
