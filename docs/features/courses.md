# Courses

## Intent and user flow

Course mode plays an ordered set of charts under shared constraints and then
records a result that can be browsed, recalled, replayed, or exported. Course
identity follows content rather than a transient difficulty-table navigation
identifier, so history can survive table churn.

## Code map

- `src/CourseIdentity.*` derives the durable content key from ordered chart
  identities and semantic constraints.
- `src/replay/CourseContinuation.*`, `CourseReplay*`, and
  `CourseResultPersistence.*` carry stage state, replay capture, and recovery.
- `src/scene/play/` continues active stages; `ResultScene` and `MainMenuScene`
  persist and present course outcomes.
- `src/repositories/ReplayRepository*` stores strict modern course results,
  stages, entries, and their replay associations.

## Boundaries and invariants

Course result data and replay files have separate ownership and recovery paths.
Continuation preserves the stage order and effective rules such as long-note
mode; invalid optional replay detail must not destroy an otherwise valid course
result. Modern course records use the content identity before numerical table
IDs.

## Verification

Begin with `course_identity_tests`, `course_continuation_tests`,
`course_replay_*_tests`, `practice_result_flow_tests` where applicable, and
the course cases in repository and result-persistence integration tests.

## Related pages

- [Gameplay and scoring](gameplay-and-scoring.md)
- [Replays and video export](replays-and-video-export.md)
- [Results, records, and persistence](results-records-and-persistence.md)
