# IR Upload Failure Reasons Design

## Goal

Show a useful failure reason on each affected attempt row in the `IR Uploads`
page. The row covers both immediate local verification or batch-queue failures
and later provider delivery failures. The page continues to queue verified
scores through one batch operation rather than issuing one request per score.

## User Experience

Each attempt row gains one bounded line beneath its existing attempt metadata.
When the attempt has a failure reason, the line reads
`Failed: <sanitized reason>` in the existing coral failure color. When no
reason exists, the line is empty and cannot retain text from a previously bound
recycled row.

The reason follows these lifecycle rules:

- a verification or batch-queue failure appears as soon as preparation ends;
- retrying and failing replaces the previous session reason with the newest
  reason;
- successfully queueing the attempt clears its session reason;
- refreshing candidates during the same page session preserves immediate
  reasons for attempts that remain visible;
- leaving and reopening the page clears immediate reasons;
- provider delivery reasons survive page reopen because they already belong to
  the durable outbox row.

If both a durable outbox reason and a newer session reason are available, the
session reason wins. It represents the most recent action the user performed.

## Data Model and Boundaries

`ReplaySummary` gains the provider outbox's last sanitized error message for
the candidate read. `ListIrUploadCandidateReplays` selects
`ir_outbox.last_error_message` alongside the failed outbox state and validates
that it is nullable text. The candidate query still publishes only attempts
with no outbox row or a `FailedPermanent` row, so this extra value cannot make
queued, uploading, awaiting, blocked, or uploaded work selectable.

`IrUploadCandidate` gains a presentation-only failure reason. Candidate
projection copies the durable outbox reason into this field after sanitizing
and bounding it. Views consume the candidate field without reading a
repository or submission service.

Immediate failures remain page-session state in `IrUploadsController`. The
controller owns a replay-ID-to-diagnostic map and reapplies matching values
whenever candidates are replaced after a refresh. This keeps refresh and
virtualized row binding deterministic without persisting verification failures
or creating a second scene-owned row model.

## Preparation Outcomes

`PreparationOutcome` carries a bounded per-replay failure collection in
addition to its queued and failed replay IDs. `prepareSelectedCandidates`
records a sanitized reason at the point each failure becomes known:

- replay loading or result reconstruction uses the specific safe diagnostic
  from `BuildChartResult` when available;
- a reconstructed result without verifiable historical IR proof uses a concise
  fallback explaining that the saved result has no verifiable IR proof;
- a verifier exception uses a generic verification-failed fallback and does
  not abort other candidates;
- draft construction and batch enqueue failures use the corresponding
  per-attempt `IrSavedResultBatchUploadResult` diagnostic;
- missing, duplicate, or malformed batch outcomes use explicit fail-closed
  fallback text;
- an unavailable verification or enqueue dependency uses an explicit
  unavailable fallback.

Diagnostics remain subject to the existing IR sanitization and 512-byte bound.
They must not include API keys or other credential material.

Cancellation is not presented as a per-row failure. Cancellation keeps every
unqueued row selected and retains any failure reason that was already visible,
while the existing page-level text reports `Upload cancelled.`

## Controller Merge Rules

On non-cancelled completion, the controller:

1. removes session reasons for every queued or concurrently handled replay;
2. replaces session reasons for failed replays that have a new diagnostic;
3. keeps failed replays selected;
4. keeps the existing queued/failed page summary.

On candidate refresh, the controller intersects selection as it does today,
removes session reasons for replays no longer published, and applies remaining
session reasons over durable candidate reasons. Session state disappears with
the controller when the page closes.

## Row Layout

`IrUploadCandidateListItemView` adds a named `TextView` for the failure reason
under the attempt line. The existing fixed-height row has sufficient vertical
space for one additional 18-pixel line. The new text uses hidden overflow and
the coral themed color; the full diagnostic remains bounded before it reaches
the view.

Every bind sets the failure text, including setting it to an empty string for
rows without failures. This is required so recycler reuse cannot leak another
attempt's diagnostic.

## Testing

Repository tests cover loading and sanitizing a durable failed-outbox
diagnostic and leaving the value empty when no failed outbox row exists.
Candidate projection tests cover copying the durable reason only onto the
matching candidate.

Preparation and controller tests cover:

- retaining the verifier's specific diagnostic;
- supplying a safe fallback for verifier exceptions;
- mapping reordered batch item diagnostics by attempt ID;
- fail-closed diagnostics for missing and duplicate batch outcomes;
- preserving session reasons across candidate refresh;
- replacing a reason after another failed retry;
- clearing a reason after successful queueing;
- preserving existing reasons during cancellation;
- preferring the latest session reason over a durable reason.

View tests cover showing the failure line, clearing it on recycled-row rebind,
and preserving the existing chart, attempt, jacket, score, status, and
selection presentation.

Final verification runs the focused repository, candidate projection,
controller, and list-view targets, the full configured CTest suite,
`git diff --check`, and
`cmake --build cmake-build-debug --target main -j 6`. No Firebase deployment is
part of this change.

## Out of Scope

- Persisting local verification or batch-preparation diagnostics across page
  sessions.
- Adding a modal, toast, or separate failure-details page.
- Changing provider retry policy, remote reconciliation, or outbox state
  semantics.
- Changing the provider-native batch request grouping or issuing one request
  per score.
