# IR Proof Rejection Analysis Design

## Goal

Replace the generic `This saved result has no verifiable IR proof.` failure with a sanitized explanation that identifies both why historical IR verification failed and what that means for the user.

## Scope

- Analyze failures while reconstructing the historical IR proof for a saved chart result.
- Preserve the analysis through `ResultRecallBuilder` and the IR Uploads verifier.
- Display the explanation in the existing attempt-detail line as `Failed: <cause> <remediation>`.
- Keep immediate verification diagnostics session-only, matching the current IR Uploads behavior.
- Do not change candidate eligibility, batch request behavior, outbox persistence, or imported-score persistence.

## Diagnostic Model

`result_recall::ChartResult` gains a diagnostic for the case where chart reconstruction succeeds but historical IR proof reconstruction does not. A successful historical proof has an empty diagnostic. A missing proof has exactly one cause and, where useful, a short consequence or remediation.

The historical-proof builder distinguishes these stages:

1. Stored integrity metadata is missing or invalid.
   - Missing attempt identity.
   - Missing or empty stored fingerprint.
   - Missing play-completion timestamp.
2. The canonical score attempt cannot be reconstructed.
   - Preserve the existing safe invariant diagnostic, such as chart identity, final gauge, or clear-type mismatch.
3. The reconstructed attempt fingerprint differs from the stored fingerprint.
   - Explain that chart or replay metadata may have changed since the score was saved and that the score cannot be uploaded safely.
4. The reconstructed attempt cannot become an IR submission.
   - Preserve the existing safe submission-construction diagnostic and explain that the saved score cannot be uploaded safely.

The user-facing forms are deterministic:

- `IR verification failed because the saved result has no attempt identity. This score cannot be uploaded safely.`
- `IR verification failed because the saved result has no integrity fingerprint. This score cannot be uploaded safely.`
- `IR verification failed because the saved result has no play completion time. This score cannot be uploaded safely.`
- `IR verification failed: <safe invariant>. The saved replay no longer reproduces the original score, so it cannot be uploaded safely.`
- `IR verification failed because the stored fingerprint differs from the reconstructed score. The chart or replay metadata may have changed since the score was saved, so it cannot be uploaded safely.`
- `IR submission validation failed: <safe invariant>. This score cannot be uploaded safely.`

Diagnostics must describe invariant names only. They must never include chart titles, paths, hashes, UUIDs, replay payloads, API keys, credentials, or exception text. The IR Uploads controller remains the final sanitization and byte-boundary layer.

## Data Flow

1. `ResultRecallBuilder` reconstructs the chart and replay outcome as today.
2. Historical IR proof reconstruction returns either a `HistoricalIrContext` or a sanitized diagnostic.
3. `ChartResult` carries the diagnostic when the chart result is usable but historical IR proof is unavailable.
4. `IrUploadsScene` returns that diagnostic from its verification callback instead of the generic no-proof message.
5. The controller retains the failure for the current page session.
6. The candidate row appends `Failed: <diagnostic>` to its timestamp/combo/gauge detail line.

## Error Handling

- Missing metadata receives a deterministic user-facing explanation rather than an empty diagnostic.
- Existing attempt/submission invariant diagnostics are wrapped in contextual language and sanitized.
- Fingerprint mismatch receives an explicit likely-cause explanation without disclosing either fingerprint.
- Unexpected exceptions retain the existing safe reconstruction fallback and do not expose `what()`.
- Cancellation continues to produce no per-row failure reason.

## Testing

- Extend `result_recall_builder_tests` to require precise diagnostics for:
  - missing attempt identity;
  - missing/empty fingerprint;
  - missing play timestamp;
  - canonical attempt reconstruction rejection;
  - stored/reconstructed fingerprint mismatch;
  - IR submission construction rejection.
- Assert successful historical IR reconstruction has no diagnostic.
- Extend `ir_uploads_flow_audit` to require the scene to forward the historical-proof diagnostic and to reject the obsolete generic no-proof literal.
- Retain the candidate-row suffix and recycling tests.
- Run focused tests, the desktop `main` build, and the complete configured CTest suite.

## Non-Goals

- Repairing or rewriting old replay records.
- Uploading scores whose stored proof cannot be reproduced.
- Adding new database columns or migrations.
- Sending one network request per score.
- Persisting local verification explanations beyond the current IR Uploads page session.
