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

Pausing a live single-chart attempt after the first note is reached and before
all notes have been judged or missed marks it as modified assisted play, excludes
it from Internet Ranking and best-record updates, and caps successful clears at
LIGHT ASSIST EASY. Saved results and replays retain this fact. Resume keeps the
penalty; Retry and Retry Same start fresh attempts without it. The course menu
does not pause the song and is exempt, as is pausing replay viewing. This uses
the existing assist-option metadata without changing the result or replay schema.
Existing records keep their original interpretation and fingerprints.

## Verification

Use `result_persistence_*_tests`, `result_record_*_tests`,
`result_presentation_model_tests`, `modern_result_recall_tests`,
`score_provenance_*_tests`, and repository integration tests.

## Related pages

- [Replays and video export](replays-and-video-export.md)
- [Courses](courses.md)
- [Internet Ranking](internet-ranking.md)
