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
