# Gameplay and scoring

## Intent and user flow

Gameplay loads an activated chart, accepts normalized logical controls, drives
audio and visual time, judges notes, and routes a completed attempt to the
result flow. The same rules must remain coherent for normal play, autoplay,
practice, replay playback, courses, and export.

## Code map

- `src/scene/play/GamePlayScene.*` owns the gameplay-scene lifecycle and
  orchestration.
- `src/scene/play/` contains timing, judging, gauge, score state, BGA targets,
  realtime worker, visual state, and playfield presentation boundaries.
- `BestReplayLoad.*` schedules saved-best replay resolution through
  `ReplayRecordTask`; `GamePlayScene` applies the loaded personal-best ghost.
- Root-level result/provenance helpers model an immutable completed attempt.
- `src/rendering/` and the built-in playfield renderer turn prepared visual
  state into frame work; they do not decide gameplay rules.

## Boundaries and invariants

Gameplay time and original chart time are explicit inputs; rate changes must
not silently rewrite chart/replay timestamps. The ruleset and score provenance
are captured for the effective attempt, after its final options and course
constraints are known. Gameplay state owns judging and submission authority;
views and renderers consume projections of that state.

Initialization, live input, and teardown are lifecycle-sensitive. Stop realtime
ingress before releasing scene-owned presentation, audio, or visual state.

Best-replay replacement cancels and joins the previous task before admitting
the next attempt. Resolver creation/loading stay on the worker; only a
successful, uncancelled result queues a scene callback. Consumption joins that
worker before applying the ghost on the application thread. Configuration,
cleanup, and destruction discard pending work. Missing/unreadable replays keep
the fallback, and the selected pacemaker target remains proportional even when
the personal-best ghost gains replay progression.

## Lane covers

Song play options and Settings → Lane expose independent **SUDDEN+**, **HIDDEN+**,
and **LIFT** switches and amounts (0–1000). They can be combined. SUDDEN+ covers
the top of the lane; HIDDEN+ covers the bottom above the judgement line; LIFT
raises the judgement line and shortens the scrolling lane. Both covers use the
lane height remaining above LIFT. These are Beatoraja's adjustable covers;
there is no separate classic fading HIDDEN/SUDDEN mode in the reference player.

Default keyboard controls are **Q = START**, **W = SELECT**, **Up/Down = move
cover**. Defaults yield to configured actions or occupied keys. Mouse wheel also
moves the cover. Hold START and turn scratch to adjust in fine steps; holding a
digital scratch accelerates after 500 ms. Double-tap START to toggle SUDDEN+.
When SUDDEN+ is off, adjustment targets LIFT, or HIDDEN+ if LIFT is off. With both
lower modes enabled, START+SELECT switches which one is adjusted (release promptly;
holding both is the existing exit shortcut). START+keys changes hi-speed;
SELECT+keys/scratch changes the green number.

Built-in rendering and compatible Lua/LR2 skins receive the same cover state,
including fine white-number values, lift/hidden offsets and enabled options.
Skins still supply their own cover artwork. Replays record initial amounts and
subsequent adjustments; older replays retain their existing fallback behavior.
NOSPEED courses suppress cover amounts and their adjustment controls.

## Built-in keybombs

Judged lanes display short procedural bursts at the judgement line. PGREAT
uses a layered halo and sparks; GREAT uses a ring and sparks; GOOD uses a soft
pulse. Successful hits use a bright core, thicker rings and sparks, and a sustained
fade for clear feedback. BAD fragments rise, KPOOR uses a subdued ripple, and
POOR never renders keybombs.
Default colors match judgement text in both light and dark themes. No external
skin artwork is required.

Settings → Skins → Built-in → Effects → Keybombs and the built-in preview's
Lane controls expose per-mode settings with a judgement selector, visibility, size (25–300%),
opacity (0–100%), and color presets, picker, and hex input. Reset color restores
the original palette for that judgement. Each judgement is independent, and landscape/portrait
presentations retain separate settings. The preview's random timing mode
exercises poorer judgements as well as successful hits. An inline animated
preview shows the selected judgement’s burst and hold sparks side by side and
responds to appearance edits. POOR is excluded from the keybomb selector.

`LaneEffects.h` shares the preallocated lane state with beams. A keybomb event
publishes its judgement and timestamp with one lock-free atomic store; it
adds no waits, allocations, particle simulation, or graphics calls to input
callbacks. The render side samples both effects once per lane, generates
bounded geometry, and submits batches. The newest judgement replaces a lane's
previous burst. Empty presses/releases preserve it, reset clears it, and the
visual clock controls expiry in live play, autoplay, preview, and replay.
Holding an LN, CN, or HCN sustains a pulsing glow with staggered, rotating sparks
using the selected judgement style. Release, a broken hold, or reaching the tail
stops the continuous emitter; HCN regrabs restart it. The short judgement burst
retains its own lifetime. Holds without a head judgement use the PGREAT style.
The emitter reuses render traversal and fixed geometry, with no extra input work.
Beams can be disabled independently. Custom skins keep their own effects.

`lane_effects_tests` checks coherent publication, zero producer allocations,
geometry, expiry, moving hold particles, and style controls. The captured-state and headless Metal
renderer tests cover misses, preview/replay handoffs, reset, and actual pixels.
These checks do not replace performance measurements on a mobile device.

## Verification

Use `gameplay_ruleset_tests`, `gameplay_score_state_tests`,
`gameplay_simulation_tests`, `gameplay_playback_startup_tests`,
`gameplay_automatic_authority_tests`, `realtime_gameplay_worker_tests`, and
the focused gauge/judge/playfield test targets.
`best_replay_load_tests` covers asynchronous resolution and the production
scene handoff; `best_replay_resolver_tests`, `replay_record_task_tests`, and
`replay_summary_list_tests` cover the underlying resolution, ownership, and
pacemaker policy.

## Related pages

- [Courses](courses.md)
- [Practice and analysis](practice-and-analysis.md)
- [Input and controllers](input-and-controllers.md)
- [Gameplay skins](gameplay-skins.md)
