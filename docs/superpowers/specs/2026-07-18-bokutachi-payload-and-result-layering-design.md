# Bokutachi Payload and Result Layering Design

## Goal

Correct the result-screen gauge graph's render layer, submit gauge history to
Bokutachi, exclude PGREAT timing from Bokutachi FAST/SLOW metrics, and display
active deferred-import polling separately from initial score submission.

## Scope

This change applies to single-chart Bokutachi/Tachi Direct Manual submissions
and the live result screen. It does not change local FAST/SLOW totals, score
database columns, replay persistence, ranking behavior, other result exports,
or the durable meaning of existing outbox states. API keys remain profile
credentials and never enter payload or outbox rows.

## Gauge Graph Layering

The result graph currently submits a `SimpleBatchRenderer` batch from
`ResultScene::renderScene()` after the root View tree has already rendered.
The UI view uses sequential submission order, so this late batch paints over
the rankings modal even though the modal's portal has a higher View z-index.

The graph will render through a dedicated `ResultGaugeGraphView` attached as a
child of the skin's existing `graph` placeholder. Its `renderImpl()` will use
the current render transform and scissor, and it will preserve the existing
colors, guides, markers, gauge-profile scaling, and responsive placeholder
geometry. Because the graph is then part of the View traversal, the root
overlay portal naturally renders after it. `ResultScene::renderScene()` will no
longer submit a graph batch and will only keep the overlay portal sized to the
viewport.

## Canonical Submission Evidence

`IrSubmission` will retain three additional pieces of provider-neutral result
evidence:

- Gauge history snapshots derived from persisted replay events that mutate the
  gauge: judged press/release/miss events, mines, and explicit gauge events.
- The number of early PGREAT judgements.
- The number of late PGREAT judgements.

`makeIrSubmission()` will derive this evidence from the attempt's replay while
retaining the existing aggregate FAST/SLOW totals from the local score. Empty
history remains valid and means the optional Bokutachi metric is omitted.
Every gauge sample must be finite; values are clamped to the BMS 0–100 percent
range when mapped to the provider payload. PGREAT timing counts must be
nonnegative and may not exceed their corresponding aggregate FAST/SLOW count.

This keeps the replay as the durable source of detailed evidence, avoids new
score-database columns, and leaves local result presentation unchanged.

## Bokutachi Mapping

The Tachi Batch Manual mapper will emit `optional.gaugeHistory` when history is
available. It will first serialize the complete per-mutation history. If the
document exceeds the existing 64 KiB provider limit, it will deterministically
downsample by uniformly selecting ordered indices while always retaining the
first and final samples. It will select the largest sample count whose complete
serialized document fits. If even the minimum endpoint history cannot fit, the
draft is rejected by the existing payload-size error path.

The submitted timing metrics will be:

- `fast = local fast - early PGREAT`
- `slow = local slow - late PGREAT`

No local counter is changed. Bokutachi therefore receives only non-PGREAT
early/late judgements, as requested, while AsoBMaShow can continue displaying
the full timing totals and per-judgement breakdown.

The frozen JSON, including the selected gauge-history samples, remains covered
by the existing ruleset validation fingerprint and stored durably in the
outbox. Credential handling is unchanged.

## Submission and Polling Presentation

The durable `Uploading` claim state is intentionally shared by a fresh POST and
a poll claim. The worker chooses the correct operation from the row's state
before claiming it, but the current UI snapshot publishes only `Uploading` and
therefore labels an active poll as `Submitting`.

`IrAttemptStatusSnapshot` will add an in-memory active-request kind with three
values: none, submit, and poll. Snapshot construction will classify an
`Uploading` row with a validated remote job/origin pair as an active poll and
an `Uploading` row without remote identity as an active submit. No outbox enum
or database migration is needed; crash recovery continues distinguishing the
two cases with the same persisted remote identity.

Result presentation will show:

- `Submitting` while the initial POST is active.
- `Waiting for Bokutachi` while a 202 import is waiting for its next poll.
- `Polling Bokutachi` while that poll request is active.
- `Submitted` after the import succeeds.

Retry behavior and remote-job preservation remain unchanged.

## Error Handling

- Non-finite gauge-history samples make canonical submission construction
  fail closed before an outbox draft is created.
- Invalid PGREAT timing breakdowns make the provider draft invalid rather than
  clamping a negative submitted count.
- Gauge-history downsampling is deterministic so retries and ruleset proof
  fingerprints remain stable for identical attempts.
- Payload-size, credential, transport, 202 polling, and permanent-failure
  handling retain their current behavior.

## Testing

- Extend the result visual layout audit to require the graph to be attached to
  the View tree and to reject graph submission from
  `ResultScene::renderScene()`.
- Add canonical submission tests for gauge-mutation extraction, PGREAT
  early/late extraction, empty history, and non-finite history rejection.
- Add Tachi Batch Manual tests proving complete history is emitted when it
  fits, oversized history is deterministically reduced with endpoints
  preserved, the final payload stays within 64 KiB, and submitted FAST/SLOW
  exclude PGREAT.
- Add submission-service tests proving a blocked initial POST publishes submit
  activity and a blocked deferred poll publishes poll activity.
- Add result-presentation tests for the new active polling label and for the
  existing waiting/submitting/submitted labels.
- Run the focused tests, the desktop `main` build, and the complete CTest suite.

## Reference

Tachi documents `gaugeHistory` as an optional array of gauge-percent snapshots
and defines FAST/SLOW as early/late mistakes for BMS 7K and 14K:
<https://docs.tachi.ac/game-support/games/bms-7K/>.

