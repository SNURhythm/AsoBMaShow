# Results, records, and persistence

## Intent and user flow

After an attempt, the Result scene presents its score, gauge, judgments,
provenance, and related actions. Records in the main menu combine current
local results, verified replay availability, and eligible remote records into
bounded, filterable history. Users can recall a result even when the replay is
not needed or is unavailable.

## Code map

- Root result types such as `ModernResult`, result-record summaries, score
  provenance, and persistence models define durable result facts.
- `src/scene/ResultScene.*`, result presentation helpers, and main-menu record
  composition own scene behavior and views.
- `src/repositories/ScoreRepository*` and `ReplayRepository*` own SQLite
  schemas, migrations, result rows, records, and recovery work.
- `src/ModernResultRecallBuilder.*` reconstructs result presentation from a
  stored result and current chart identity without treating replay loading as a
  prerequisite.

## Boundaries and invariants

Results, replay files, and Internet Ranking submission snapshots persist
independently and link through validated identities. Schema migration is
versioned and must fail without destructive mutation on unknown/future data.
Record lists are bounded projections, not a signal to eagerly hydrate replay
payloads. User actions that delete files or retrigger persistence require a
specific confirmation/lifecycle boundary.

## Verification

Use `result_persistence_*_tests`, `result_record_*_tests`,
`result_presentation_model_tests`, `modern_result_recall_tests`,
`score_provenance_*_tests`, and repository integration tests.

## Related pages

- [Replays and video export](replays-and-video-export.md)
- [Courses](courses.md)
- [Internet Ranking](internet-ranking.md)
