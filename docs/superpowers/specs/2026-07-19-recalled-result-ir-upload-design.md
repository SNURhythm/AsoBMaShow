# Recalled Result IR Upload Design

## Goal

Make an eligible saved single-chart result expose the same Bokutachi controls
after it is reopened from Records as it did immediately after play. Also make
the amber `IR ↑` marker an explicit action that can queue or retry that saved
result without leaving the Records modal.

Course records, Auto Play entries, and ineligible or unverifiable scores remain
outside Bokutachi submission scope.

## Root Cause

The Records marker and recalled result use different proof depths. The marker
checks canonical attempt metadata plus stored provenance, so it can identify a
potentially uploadable row without loading the replay. The recalled result must
rebuild the complete historical attempt and compare its calculated fingerprint
with the stored fingerprint before it may expose submission controls.

Replay rows persist only the chart fields required for database identity and
display. During recall, the chart is reparsed with the replay's recorded random
sequence, play options, and long-note mode, but the builder currently copies
only the parsed path back into `ReplayData::chartMeta`. Other fingerprinted
metadata can therefore remain incomplete or differ from the original parsed
chart. The exact fingerprint comparison fails, `HistoricalIrContext` is
silently omitted, and `ResultScene` correctly hides its IR block even though
the lightweight Records marker is amber.

The fix must restore reconstructable metadata rather than weaken or bypass the
stored fingerprint comparison.

## Recalled Result Reconstruction

After `prepareReplayChart` succeeds, the result-recall builder replaces the
replay's chart metadata snapshot with the complete metadata from the parsed and
replay-configured chart before rebuilding `RhythmState` and the historical
attempt. Parsing already uses the stored parser random data, applies both
recorded play options, and applies the recorded long-note mode, matching the
chart snapshot used when the original result attempt was captured.

The existing validation sequence remains authoritative:

1. Rebuild the result state from stored replay events and the recorded ruleset.
2. Build a candidate `ChartResultAttempt` from the reconstructed chart, state,
   provenance, and replay.
3. Compare the candidate fingerprint exactly with the stored canonical
   fingerprint.
4. Create `HistoricalIrContext` only after the comparison succeeds.

A mismatch still fails closed and suppresses only IR controls. The saved result
screen remains viewable. No legacy record is upgraded, and no database row is
rewritten during recall.

## Amber Badge Interaction

The amber marker becomes a compact `Button` whose content remains `IR ↑` and
whose colors and width stay unchanged. The button is bound to the current
virtualized `ReplaySummary`; recycled rows must replace the bound summary and
action every time they are rebound.

Tapping the marker consumes the pointer event instead of selecting or opening
the row. The Records modal stays visible. A single operation guard prevents a
second marker tap, result recall, playback, or export from starting while the
saved result is being prepared.

The deferred action uses the same preparation path as `View Result`:

1. Load `ReplayResultRecord` and its stored attempt metadata from the replay
   database snapshot.
2. Parse the chart and call `result_recall::BuildChartResult`.
3. Require a validated `HistoricalIrContext` and build the provider draft
   through the registered Bokutachi driver.
4. If no outbox row exists, call `IrSubmissionService::enqueueManual`.
5. If a retryable outbox row already exists, call the existing service retry
   operation for that row instead of attempting a duplicate insert.
6. If delivery is already active or already queued, do not create a duplicate;
   report its current state and wake the submission service when appropriate.

The outbox remains the only durable network-work source. The action never reads
an API key, constructs Tachi JSON itself, or copies credentials into replay or
outbox data.

## Feedback and Refresh

While preparation is active, the tapped badge is disabled and displays a
bounded progress label such as `IR ...`. Other record actions are disabled by
the shared modal-operation guard.

After an enqueue or retry request is accepted, the modal refreshes the affected
replay summaries from the repository and restores the selected row and scroll
position. The amber marker follows the existing policy: it remains visible
while an upload is absent, queued, active, blocked, or failed, and disappears
only after the outbox state becomes `Succeeded`. Immediate modal feedback says
`Queued`, `Retry queued`, or the current active state so retaining the marker
does not look like a missed tap.

If preparation, fingerprint verification, draft construction, provider
availability, enqueue, or retry fails, the modal stays open, the marker
remains, and a sanitized concise diagnostic is shown. A disabled provider or
unavailable submission service produces an explicit Settings/service message.
Diagnostics must not contain replay payloads, credentials, or unbounded server
text.

## Code Boundaries

- `ResultRecallBuilder` owns complete replay reconstruction and historical
  attempt verification for both result viewing and marker upload.
- `ReplaySummaryListView` owns only marker presentation and emits a semantic
  upload request for the currently bound summary.
- `MainMenuScene` coordinates the deferred operation, modal guard, feedback,
  outbox lookup, enqueue/retry call, and list refresh.
- `IrDriverRegistry` remains responsible for draft construction.
- `IrSubmissionService` remains responsible for durable enqueue, retry, worker
  wake-up, credentials at send time, and delivery.

No alternate submission pipeline or result-scene dependency is introduced.

## Error and Concurrency Rules

- Auto Play and course rows never expose the marker action.
- A row without canonical attempt identity, fingerprint, positive play time,
  verified provenance, or a matching reconstructed fingerprint cannot mutate
  the outbox.
- Pointer events on the child badge do not also trigger RecyclerView row
  selection.
- Virtualized row reuse cannot retain another replay's callback or in-progress
  label.
- The operation guard is cleared on every success, failure, cancellation, and
  scene cleanup path.
- Existing outbox rows remain authoritative. An active row is never replaced
  with a newly generated payload.
- The current profile's driver settings and submission service are checked at
  action time.

## Testing

Focused regression coverage will include:

- A database-shaped replay containing only persisted chart metadata, combined
  with a complete reparsed chart, reconstructs the original attempt fingerprint
  and publishes `HistoricalIrContext`.
- A genuinely changed parsed chart or replay still fails the exact fingerprint
  check and does not expose IR context.
- A recalled result with validated historical context reaches the normal
  `Not submitted`, queued, failed, or submitted IR presentation according to
  its outbox snapshot.
- The amber marker is an interactive child control and emits the currently
  rebound replay identity without also selecting the row.
- A marker tap with no outbox row builds and enqueues one manual draft.
- Existing retryable rows use retry; existing active rows do not create
  duplicates.
- Double taps and conflicting modal actions are ignored while preparation is
  active.
- Successful actions refresh modal state while failures retain the marker and
  display sanitized feedback.

Verification includes focused result-recall, IR presentation, submission
service, replay repository, and view tests; the complete configured CTest
suite; `git diff --check`; and the desktop `main` build.

## Out of Scope

- Course score submission.
- Uploading Auto Play, practice, assisted, modified, Beatoraja, or legacy
  unverified results.
- Relaxing attempt fingerprint validation or migrating legacy records.
- Waiting synchronously in the modal for remote Tachi polling to finish.
- Changing Bokutachi payload, credential, polling, or retry policy.
