# Result Conflict Diagnostics Design

## Context

The Result screen now offers a **Show Details** modal for save outcomes in
`UnstagedConflict` or `PendingConflict`. The modal reads the internal
`SaveOutcome::diagnostic`, but several dependency conflict statuses can legally
arrive with an empty diagnostic and score-payload comparisons currently report
only a generic mismatch.

## Scope

This change covers every conflict path that can produce the current Result
screen's `UnstagedConflict` or `PendingConflict` outcome. Background recovery
conflicts are explicitly out of scope because they do not open this modal.
The multi-BAD counting behavior itself is not changed in this pass.

## Diagnostic Behavior

- Every Result-screen conflict outcome has a non-empty diagnostic.
- Dependency conflicts are labeled by phase: staging, pending-score read,
  score projection, or acknowledgement.
- If a dependency supplies a diagnostic, the coordinator preserves it and adds
  the phase context. If it supplies none, the coordinator emits a phase-specific
  fallback rather than the modal's generic no-diagnostic message.
- Coordinator-owned identity, receipt, timestamp, and payload checks describe
  the failed invariant and include expected/actual scalar references where
  useful.
- Score payload mismatches use one shared pure comparison helper. It lists every
  differing field. Numeric fields include expected and actual values so a BAD
  mismatch is directly visible; textual identity and provenance fields are
  named without dumping large or potentially private contents.
- Public warning copy remains unchanged. Detailed diagnostics appear only after
  the user selects **Show Details** and continue to be written to the existing
  diagnostic log.

## Components and Data Flow

1. `ResultPersistenceModel` provides the pure score-difference description used
   by persistence layers.
2. `ResultPersistenceCoordinator` attaches phase context and guaranteed
   fallbacks to every Result-screen conflict outcome.
3. Existing repository diagnostics remain the source for storage-specific
   integrity failures. Generic score collision messages adopt the shared
   field-difference description.
4. `ResultScene` continues to render the final diagnostic without interpreting
   it.

## Testing

- A table-driven model test mutates each score field and verifies that the
  shared comparison identifies it, including expected/actual BAD counts.
- Coordinator tests inject empty and non-empty diagnostics for each dependency
  conflict status and verify phase context plus non-empty output.
- Existing malformed receipt, pending identity, timestamp, and payload tests
  verify the more specific coordinator diagnostics.
- Focused persistence tests, persistence audits, and the desktop target must all
  pass before handoff.
