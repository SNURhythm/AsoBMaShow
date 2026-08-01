# IR Uploads Final Fix Report

Date: 2026-07-20
Branch: `feature/bokutachi-ir`
Starting revision: `800aa8e`

## Scope

This pass closes the final concurrency and scaling findings for the IR Uploads
flow:

- prevent a stale manual selection from recreating work after reconciliation
  has already persisted an exact provider/origin/attempt receipt;
- settle eligible outbox rows that are enqueued after reconciliation planning
  but before the snapshot transaction applies;
- preserve deferred work pinned to another remote origin;
- replace the saved-result outcome and queued-replay quadratic scans with
  indexed lookups; and
- add deterministic maximum-bound operation-count coverage at 16,384 rows.

No Firebase deployment or other external mutation was performed.

## TDD Evidence

The regression tests were added first. The initial focused build was run with:

```sh
cmake --build cmake-build-debug --target \
  ir_saved_result_batch_upload_tests ir_uploads_controller_tests \
  replay_repository_tests ir_submission_service_tests -j 6
```

It failed at compile time for the intentionally missing interfaces:

- `ir::detail::IrManualBatchOutcomeIndex`;
- `ir_uploads::detail::eraseQueuedReplayIds`; and
- the origin-aware four-argument `EnqueueReadyIrOutboxDrafts` overload.

After implementation, the same four targets compiled and the focused CTest
run passed 4/4:

```sh
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(ir_saved_result_batch_upload_tests|ir_uploads_controller_tests|replay_repository_tests|ir_submission_service_tests)$' \
  -j 4
```

Observed results: saved-result batch 1.06 s, uploads controller 1.45 s,
submission service 4.22 s, replay repository 24.99 s; zero failures.

## Implementation

### Receipt-aware manual enqueue

`IrSubmissionService::enqueueManualBatch` now captures the normalized origin
of the enabled active provider while holding the service mutex. The repository
batch API requires that exact normalized origin. Inside its existing
`BEGIN IMMEDIATE` transaction, the repository checks the
provider/origin/attempt receipt identity before insertion or retry. An exact
receipt returns `AlreadySubmitted` without creating or mutating an outbox row;
invalid receipt cardinality fails the transaction closed.

The singular `enqueueManual` adapter remains routed through the batch API, so
it inherits the same receipt protection.

### Reconciliation/apply race

Snapshot apply still validates and settles the row IDs named by the pure
reconciliation plan. In the same immediate transaction, it now also removes
newly represented rows that meet every one of these conditions:

- exact provider scope;
- `Pending`, `BlockedConfiguration`, or `FailedPermanent` state;
- `local_result_ready=1`;
- unpinned origin or an exact remote-origin match; and
- an exact provider/origin/attempt receipt exists.

This catches manual enqueue between plan and apply without touching uploading,
awaiting-remote, succeeded, ineligible, cross-provider, cross-origin, or
other-origin-pinned work. No remote score ID is synthesized.

### Linear mappings

`IrSavedResultBatchUpload` now builds one attempt-ID index for repository batch
outcomes and performs one lookup per prepared draft. Missing and duplicate
attempt outcomes remain failures; unknown outcomes cannot be assigned to a
different attempt.

`IrUploadsController` now builds an `unordered_set<int>` of queued replay IDs
before filtering the failed IDs. Both paths expose test-only logical operation
counts and are covered at the configured maximum of 16,384 rows, avoiding
timing-dependent performance assertions.

`replay_repository_tests` now links `IrScoreReconciliation.cpp` so the
plan-before-apply race can be exercised through the real planner and real
repository transaction.

## Regression Coverage

New coverage verifies:

- a manual batch returns `AlreadySubmitted` and leaves zero outbox work after
  an exact active-origin snapshot receipt settled a stale UI selection;
- a row inserted after reconciliation planning is removed by snapshot apply;
- maximum-bound reversed outcome mapping stays exact with `2N` logical
  operations;
- maximum-bound queued/failed filtering preserves failed input order with
  `N + N/2` logical operations; and
- missing, duplicate, and unknown saved-result outcomes cannot be mismapped.

## Final Verification

Broader build command:

```sh
cmake --build cmake-build-debug --target \
  main ir_saved_result_batch_upload_tests ir_uploads_controller_tests \
  replay_repository_tests ir_score_reconciliation_tests \
  ir_submission_service_tests ir_driver_tests \
  tachi_user_score_parser_tests tachi_batch_manual_tests tachi_driver_tests \
  bokutachi_cache_store_tests ir_upload_candidates_tests \
  ir_upload_candidate_list_view_tests ir_saved_result_upload_tests -j 6
```

Result: exit 0; the desktop `main` executable linked.

Relevant tests and audits:

```sh
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(main_menu_settings_anchor_audit|ir_uploads_flow_audit|repository_boundary_tests|result_persistence_flow_audit|ir_driver_tests|ir_receipt_models_tests|ir_upload_candidates_tests|ir_remote_score_models_tests|ir_score_reconciliation_tests|tachi_user_score_parser_tests|tachi_batch_manual_tests|tachi_driver_tests|bokutachi_cache_store_tests|ir_submission_service_tests|ir_saved_result_upload_tests|ir_saved_result_batch_upload_tests|replay_repository_tests|ir_upload_candidate_list_view_tests|ir_uploads_controller_tests)$' \
  -j 6
```

Result: 19/19 passed, zero failures, 24.53 s real time.

Complete configured suite:

```sh
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Result: 128/128 passed, zero failures, 25.46 s real time.

`git diff --check` also completed with no output and exit 0.

## Self-review and Concerns

- Receipt lookup and enqueue/retry share one immediate transaction.
- Receipt and dynamic settlement scopes both require exact provider and
  normalized server origin.
- Deferred rows pinned to another origin are retained.
- Dynamic settlement is restricted to locally ready, non-active retry states.
- Existing succeeded-row purge continues to require its stronger replay and
  receipt evidence.
- No new credential persistence, logging, guessed remote identity, network
  request, deployment, or schema migration was introduced.

No functional concerns remain. The broader desktop build emitted only the
repository's existing third-party bgfx GNU variadic-macro warnings and
duplicate-library linker warnings.

## Singular already-submitted follow-up

The final re-review identified one compatibility loss at the singular service
adapter: a receipt-only batch `AlreadySubmitted` result was collapsed into
`IrOutboxInsertStatus::AlreadyExists`. The saved-result UI consequently called
the operation accepted and displayed `IR upload queued.` even though no outbox
row existed.

This follow-up adds an explicit `IrOutboxInsertStatus::AlreadySubmitted` and
preserves it only when the singular service adapter receives the matching batch
status. `IrSavedResultUpload` maps that value to the existing
`IrSavedResultUploadState::AlreadySubmitted`, `accepted=false`, and
`This score has already been submitted.` The legacy/concurrent
`AlreadyExists` path remains unchanged and continues to report queued. The
direct Result scene also treats the new status as a non-error while its normal
receipt-backed presentation refreshes.

TDD RED was verified by adding the tests first and building:

```sh
cmake --build cmake-build-debug --target \
  ir_saved_result_upload_tests ir_submission_service_tests -j 6
```

Both test targets failed to compile at the new assertions because
`IrOutboxInsertStatus::AlreadySubmitted` did not exist. After the minimal enum
and mapping implementation, the focused tests passed 2/2.

The service regression uses the real receipt-only repository fixture: snapshot
apply persists an exact-origin receipt and removes the outbox row, after which
the singular adapter returns explicit `AlreadySubmitted` with no entry. The
saved-result regression asserts the exact user-facing state, false acceptance,
and message. The existing `AlreadyExists` regression remains in place as the
compatibility guard.

Final follow-up verification:

```sh
cmake --build cmake-build-debug --target \
  main ir_saved_result_upload_tests ir_submission_service_tests \
  replay_repository_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(ir_saved_result_upload_tests|ir_submission_service_tests|replay_repository_tests)$' \
  -j 3
```

The desktop executable and all three test targets built successfully; CTest
passed 3/3 with zero failures in 25.09 s. No deployment was performed. The
only build output of note remained the existing third-party bgfx GNU
variadic-macro warnings and duplicate-library linker warnings.
