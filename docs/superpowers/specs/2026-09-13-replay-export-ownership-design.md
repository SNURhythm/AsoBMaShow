# Shared Records integration

## Scope

Complete Music Select's Records modal using the same record services as Main
Menu. Consolidate export and preparation ownership, chart and course history,
result recall, BRD sharing/deletion, and IR actions. Keep scene-specific preview
handoff and navigation explicit. Replay formats, scoring, persistent schemas,
encoding, and rendering are outside this change.

The initial export-only refactor grew to full Records integration after the user
identified the existing selector divergences as unintended. User-visible policy
differences are resolved explicitly rather than inferred from whichever
implementation is extracted first.

## Execution ownership

`ReplayExportJob` owns reservation, worker, cancellation, latest progress, and a
single terminal result. A scene reserves before handing over preview resources.
Work receives a copied options value with an owned stop token and queued
progress callback, plus the parser cancellation atomic. Both ordinary failures
and unexpected exceptions become terminal results. UI delivery joins the worker
before releasing the reservation. Destruction cancels preparation and export
before releasing either mailbox.

`ReplayRecordTask` owns cancellable preparation and its UI completion. Work
publishes an owning callback; consuming it joins preparation before navigation.
A worker that terminates without a callback still produces a terminal
notification, so cooperative cancellation cannot strand modal busy state.
Cancelling the owner joins and discards queued or late callbacks.

App backgrounding keeps Watch/result preparation running. Completion delivery
waits until foreground resume. Leaving the selector cancels pending preparation.
Music Select restores selector audio after a foreground failure or cancellation,
but does not restart it after a successful gameplay/result handoff.

Exports use worker execution and queued progress on every supported platform,
including Android. Preview workers must relinquish chart media before export
starts. Scene cleanup joins Records/export owners before destroying resources
they can access. Lua-published actions follow Records' modal ownership just as
ordinary input does.

## Shared services

- `ResultRecordsLoader` combines synthetic autoplay, legacy and modern history,
  verified replay availability, remote scores, receipts, and live IR activity.
  It returns sanitized diagnostics without publishing UI state.
- `ChartRecordActions` and `CourseRecordActions` prepare saved result views and
  own any retry/replay data. Missing or deleted replay files do not remove saved
  result history. Existing verification and retry constraints remain intact.
- `RecordFileActions` owns document handoff, retains the verified snapshot for
  asynchronous BRD sharing, and deletes replay files without deleting history.
- `RecordsIrActions` shares eligibility checks, saved-result submission, and
  status text. Submission retains the existing service's queue semantics.

Music Select snapshots a saved course's identity and stage records when opening
Records. Its modal target has no synthetic single-chart hash or path. Missing
stages do not prevent browsing history; replay and recall preparation validate
availability separately. Chart and course replay-slot shortcuts use the same
preparation paths as Records.

Local and remote result screens return to the retained originating selector.
Course Watch preserves that owner, table context, and selected pacemaker across
stage transitions. Remote results retain their read-only action and persistence
boundaries.

## Confirmed behavior decisions

| Concern | Chosen behavior |
| --- | --- |
| Export execution | Worker execution, queued progress, exception boundary |
| Autoplay export pacemaker | Retain an explicitly supplied target; otherwise use the selected target |
| Saved replay Watch pacemaker | Use the selected target |
| Autoplay Watch RANDOM | Reuse matching in-memory preloaded choices; generate fresh choices otherwise |
| Autoplay Watch and G-Battle ClubMode | Use the selected setting |
| Autoplay Watch touch/ghost rendering | Explicitly disable both |
| Watch, G-Battle, result preparation | Cancellable background work |
| Audio load failure during Watch/G-Battle | Continue unless cancelled |
| Diagnostics | Sanitized detailed messages and warnings |
| App background during preparation | Continue work; defer launch until resume |

Chart-library records do not persist RANDOM seeds or branch choices from prior
plays. In-memory preloaded chart choices and persisted replay choices are
separate inputs; neither is described as stored play history.

## Verification

Production-worker tests cover reservation, resource lifetime, one-time delivery,
latest progress, stop propagation, exception conversion, cancellation without a
result, and reuse. Repository-backed service tests cover chart/course recall,
verified replay ownership, partial courses, remote/IR projection, detached share
lifetime, failed handoff startup, deletion, and sanitized diagnostics.

Scene fixtures execute extracted production methods with real worker owners.
They cover modal recovery, audio failure/cancellation, foreground delivery,
preview ownership, Lua action gating, course navigation, and retained results.
Build the desktop app and affected native targets, run focused tests, then the
full parallel CTest suite. Desktop compilation and fixtures do not establish
physical-device export behavior.
