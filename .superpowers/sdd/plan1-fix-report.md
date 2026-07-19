# Plan 1 Review Fix Report

## Outcome

All five consolidated Plan 1 findings are addressed. Credential replacement/removal now quiesces IR work before receipt invalidation, receipt clearing removes only receipt-backed succeeded outbox evidence in one transaction, malformed successful response identities fail deterministically, records-result state is derived from current repository data, and recycled replay rows bind callbacks to the displayed replay identity.

No deployment or push was performed.

## Finding 1 — Old-profile work could commit after credential receipt clearing

**Root cause:** Credential actions invalidated receipts and changed credentials without first establishing a quiescence boundary across submission and ranking services. An already-running request could therefore complete under the old credential after the clear.

**RED evidence:** New tests initially failed to compile because `IrSettingsPresentation::Dependencies` had no `quiesceRemoteWork` or `reactivateRemoteWork` hooks. The ranking pause-state test also reported:

- `FAIL: paused ranking service rejects new work through credential mutation`
- `FAIL: profile reactivation admits ranking work again`

**Fix:**

- Added explicit quiesce/reactivate dependencies to credential replacement and removal.
- Wired Settings to `pauseIrProfileServices` before invalidation and `activateIrProfileServices` after the new credential state is committed.
- Added a reactivation guard so every failure/exception path after quiescence attempts to restore service availability.
- Made `IrRankingService` retain a paused state and reject new work until explicit activation.
- Added a deterministic integration race test whose submission driver ignores cancellation until released. It proves receipt invalidation cannot begin while old-profile work is active, then proves the old receipt-backed success is cleared and is not replayed under the new account.

**GREEN evidence:** `ir_settings_presentation_tests`, `ir_submission_service_tests`, and `ir_ranking_service_tests` pass, including replace/remove, failure-path reactivation, blocked-old-request, paused rejection, and explicit reactivation cases.

## Finding 2 — Receipt clearing left succeeded outbox evidence

**Root cause:** `ClearIrSubmissionReceipts` deleted receipt rows only. The corresponding succeeded outbox row remained and could still influence semantic state.

**RED evidence:** The expanded repository test expected two affected rows for a receipt-backed success but the prior implementation deleted only the receipt.

**Fix:** `ClearIrSubmissionReceipts` now uses one `BEGIN IMMEDIATE` transaction to delete only succeeded outbox rows that have a matching provider/origin/attempt receipt, then deletes the matching receipts. Unfinished work, other providers/origins, and legacy succeeded rows without a receipt are preserved. A trigger-induced failure test proves both deletes roll back atomically.

**GREEN evidence:** `replay_repository_tests` passes the before/after semantic-state assertions, preservation cases, affected-row count, and rollback case.

## Finding 3 — Parser accepted identities the receipt model rejected

**Root cause:** Tachi response parsing accepted score identifiers containing control bytes, while successful receipt validation rejected them. This allowed a nominal success to leave a submission claimed/uploading.

**RED evidence:** New immediate and completed-poll cases failed with:

- `FAIL: immediate score identity with a control character is malformed`
- `FAIL: completed poll score identity with a control character is malformed`

**Fix:** The Tachi parser rejects C0 and DEL control bytes in score identities. As a defense in depth, `IrSubmissionService` validates the complete successful receipt draft before applying the outcome; an invalid success is converted to a permanent `malformed_response` failure so the claim reaches a terminal state.

**GREEN evidence:** `tachi_driver_tests` and `ir_submission_service_tests` pass the control-character and invalid-success terminal-state cases.

## Finding 4 — Records result relied on stale local upload markers

**Root cause:** The result flow retained mutable local upload/replay markers whose lifetime could outlast repository changes, rather than deriving IR state from current replay identity and repository state.

**RED evidence:** Before the source-audit update, `check_records_result_recall_flow.py` failed with:

`records result recall contract failure; missing=['setIrUploadInProgress', 'cleanup:replayIrUploadReplayId.reset()'] forbidden=[]`

This demonstrated that the audit encoded the stale-marker contract.

**Fix:** Updated the contract audit to require the semantic resolver, revision observation, and current cleanup behavior, while forbidding the removed `setIrUploadInProgress` and `replayIrUploadReplayId` markers.

**GREEN evidence:** `check_records_result_recall_flow.py .` passes and CTest's `records_result_recall_flow_audit` passes.

## Finding 5 — Recycled replay-row callbacks used mutable row state

**Root cause:** The IR badge listener was installed once and delegated through mutable row fields. Recycling could change the displayed summary without rebinding the action identity.

**RED evidence:** The new view test initially failed to compile because there was no callback-bound identity accessor (`irBadgeCallbackReplayId`), making the missing identity contract explicit.

**Fix:** Every `setSummary` clears the previous listener, then installs a new callback capturing the complete bound `ReplaySummary` and handler for that visible row. Hidden badges clear listener and identity. The source audit now requires callback-bound identity and listener cleanup.

**GREEN evidence:** `replay_summary_list_view_tests` passes direct row reuse across replay IDs and action/feedback states; `check_records_ir_marker.py .` passes.

## Files Changed

- `CMakeLists.txt`
- `scripts/check_main_menu_settings_anchor.py`
- `scripts/check_records_ir_marker.py`
- `scripts/check_records_result_recall_flow.py`
- `src/ir/IrRankingService.cpp`
- `src/ir/IrSettingsPresentation.cpp`
- `src/ir/IrSettingsPresentation.h`
- `src/ir/IrSubmissionService.cpp`
- `src/ir/tachi/TachiResponseParser.cpp`
- `src/repositories/ReplayRepositoryIrOutbox.cpp`
- `src/scene/SettingsSceneIr.cpp`
- `src/view/ReplaySummaryListView.h`
- `tests/ir_ranking_service_tests.cpp`
- `tests/ir_settings_presentation_tests.cpp`
- `tests/ir_submission_service_tests.cpp`
- `tests/replay_repository_tests.cpp`
- `tests/replay_summary_list_view_tests.cpp`
- `tests/tachi_driver_tests.cpp`

## Verification

- `ctest --test-dir cmake-build-debug --output-on-failure`
  - PASS: 113/113 tests, 0 failed, 50.15 seconds.
- `cmake --build cmake-build-debug --target main -j 6`
  - PASS: desktop `main` linked successfully.
  - The build emitted existing GNU variadic-macro extension warnings from vendored `bgfx/bx` headers; no project-source error or new warning was reported.
- `python3 scripts/check_records_result_recall_flow.py .`
  - PASS.
- `python3 scripts/check_records_ir_marker.py .`
  - PASS.
- `python3 scripts/check_main_menu_settings_anchor.py .`
  - PASS: `main-menu Settings footer audit passed`.
- `git diff --check`
  - PASS.

Focused regression binaries also passed independently: `tachi_driver_tests`, `replay_repository_tests`, `ir_settings_presentation_tests`, `ir_submission_service_tests`, `ir_ranking_service_tests`, and `replay_summary_list_view_tests`.

## Self-review

- Confirmed the credential order is quiesce → invalidate receipt/cache evidence → mutate durable credential → notify committed credential → reactivate.
- Confirmed post-quiescence failures reactivate services and do not leave ranking permanently paused.
- Confirmed receipt clearing is transactionally atomic and does not delete queued/in-progress/failed work or unrelated/legacy successes.
- Confirmed a malformed nominal success cannot leave an outbox claim in Uploading.
- Confirmed recycled callbacks use the identity captured at bind time and hidden badges have no active listener.
- Confirmed no Firebase deployment, push, or parser-amalgamation edit was performed.

## Remaining Concerns

None identified. The only verification noise is the pre-existing warning from vendored bgfx macro headers noted above.
