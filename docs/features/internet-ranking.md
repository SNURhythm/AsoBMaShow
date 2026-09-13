# Internet Ranking

## Intent and user flow

Internet Ranking (IR) lets players configure a provider, authenticate securely,
inspect rankings, and upload eligible stored results. The application also
imports remote score information for read-only result recall without granting
remote records local replay or persistence actions.

## Code map

- `src/ir/IrDriver.*`, `IrHttpClient.*`, and `src/ir/tachi/` provide provider
  communication and response parsing.
- Credential backend/store/migration files provide platform-backed credential
  storage and safe migration from legacy storage.
- Submission, outbox, snapshot, receipt, candidate, ranking, and reconciliation
  types separate request construction, durable upload state, and UI projection.
- `IrUploadsScene.*`, `IrUploadsController.*`, ranking views, and result scenes
  expose the flows. `IrUploadPreparationTask.*` owns the preparation worker,
  durable-enqueue gate, and typed progress/completion handoff.

## Boundaries and invariants

An upload is based on a captured immutable submission snapshot, not a mutable
live result object. Credential values stay in platform-private storage and must
not enter logs or exported artifacts. Remote results carry a normalized
provider/origin/remote-ID identity; they are read-only and must pass exact
ranking-key validation before ranking actions appear.

Preparation cancellation first enters the durable-enqueue gate, then requests
thread stop and joins. A batch that has already begun retains its outcome;
stopped preparation retains completion for application-thread consumption.
Consuming completion joins before updating the controller. Scene reset discards
queued updates explicitly. The scene supplies external application dependencies,
while the preparation function retains verification and partial-failure policy.

## Verification

Use `ir_http_client_tests`, `ir_driver_tests`, `ir_*_tests`,
`tachi_*_tests`, credential migration/store tests, and remote result scene
tests. `ir_uploads_controller_tests` also exercises the task with controlled
verification/enqueue effects and compiles complete production launch, delivery,
and stop methods with the real controller and batch outcome mapper.

## Related pages

- [Results, records, and persistence](results-records-and-persistence.md)
- [Profiles and data transfer](profiles-and-data-transfer.md)
- [Build, release, and verification](build-release-and-verification.md)
