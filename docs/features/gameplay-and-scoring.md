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
