# IR Batch Upload Page Design

## Goal

Add an `IR Uploads` page that aggregates every locally saved single-chart
score which the current Records rules consider uploadable but which is not yet
represented on the configured IR account. Users can inspect chart and attempt
details, select individual rows or all rows, and queue the selection for
provider-native batch delivery.

The first provider remains Bokutachi through Tachi. The page includes both
freshly `Eligible` attempts and `Failed` attempts that Records exposes as
retryable. It excludes queued, uploading, awaiting-remote, blocked, uploaded,
hidden, course, Auto Play, and unverifiable attempts.

## Entry and Scene Boundary

Song Select gains an `IR Uploads` header button beside the existing header
utilities. The button opens a dedicated `IrUploadsScene`. This keeps page data,
selection, batch preparation, progress, and cancellation outside the already
large `MainMenuScene` while following the existing full-page Music navigation
pattern.

The scene has a Back action that returns to Song Select. It does not preserve
hidden modal state. Returning to or reopening the page reloads provider state,
candidate records, and chart metadata from their durable sources.

When Bokutachi is disabled, missing a credential, or blocked by configuration,
the page remains available for inspection but upload is disabled. An
`Open IR Settings` action opens Settings directly on the IR tab. Settings gains
an explicit initial-section destination; the page must not simulate tab clicks
or merely describe where the setting lives. Returning from Settings goes to
Song Select.

## Candidate Source of Truth

The replay database remains the source of attempt, provenance, outbox, and
receipt state. It gains one bounded all-chart candidate read that follows the
same integrity rules as `ListReplays`:

- decode and validate stored provenance before publishing a row;
- exclude course replay stages and Auto Play rows;
- require canonical attempt identity and fingerprint evidence;
- scope receipt evidence to provider and normalized server origin;
- preserve the current provider-scoped outbox interpretation used by Records;
- reject corrupt or oversized results with bounded aggregate diagnostics.

The repository query may exclude obviously non-actionable rows early, but it
does not weaken eligibility into a SQL-only approximation. It returns the
stored chart path and replay summary evidence required for the canonical
eligibility resolver.

The chart repository bulk-resolves candidate chart paths to full
`ChartMetaRecord` values. Full metadata supplies the stage-file jacket, title,
subtitle, artist, difficulty, key mode, hashes, and note count. Jacket metadata
is not duplicated into `replays.db`, and the scene does not scan the complete
chart library or parse every BMS file merely to render its list.

A shared candidate builder uses the same
`isReplayEligibleForBokutachi` and `resolveIrRecordState` inputs as Records.
Only semantic states `Eligible` and `Failed` become page rows. A missing chart,
changed identity, incomplete metadata, invalid provenance, or other verification
failure fails closed and contributes only to a bounded omitted-row diagnostic.

Every eligible attempt remains a separate row even when several attempts use
the same chart. Replay ID is the stable identity for selection and virtualized
row binding.

## Presentation Model and Rows

An IR upload candidate combines a local `ReplaySummary` with its resolved
`ChartMetaRecord` and semantic IR state. Views consume this presentation model
rather than querying repositories or inferring eligibility from colors.

The page header contains Back, `IR Uploads`, the current candidate count, and
Refresh. A selection toolbar contains `Select all` or `Clear selection` and an
`N selected` count. The footer contains `Upload N scores` and is disabled when
the selection is empty or provider configuration cannot submit.

Each virtualized row is a chart-list-style card with:

- a checkbox and clear-lamp strip;
- an 84-pixel jacket;
- chart title, subtitle, and artist;
- difficulty and key mode;
- attempt EX score and score rank;
- maximum combo, achieved date, and play option;
- an `Eligible` or `Retry` status.

Tapping anywhere on a row toggles its selection. Selection is stored as replay
IDs outside recycled row views. Every bind replaces checkbox state, identity,
text, image, status, callback, and selected styling so reuse cannot leak data
from another attempt.

`Select all` selects every currently published candidate. Refresh intersects
the selected replay IDs with the refreshed candidate set. The empty state says
`No scores waiting for IR upload.` Repository and chart-library failures use a
visible page-level error instead of presenting a false empty state.

## Local Batch Preparation

Starting a batch snapshots the selected replay IDs and locks row and selection
controls. The scene reconstructs and verifies each selected saved result with
the same exact fingerprint path used by recalled-result upload. This work is
local and performs no HTTP requests.

Preparation continues after an individual load, reconstruction, fingerprint,
or draft failure. Successfully verified new drafts and existing failed outbox
rows are submitted to one service batch-enqueue operation. The repository
applies the ready/retry mutations transactionally while returning bounded
per-attempt outcomes for concurrent already-queued, uploaded, missing, or
conflicting rows. A concurrent transition to queued or uploaded counts as
already handled and never creates a duplicate.

The UI reports preparation progress such as `Preparing 3 of 10…`. After the
operation, queued or concurrently handled rows disappear on refresh. Local
failures remain selected and visible. The final summary distinguishes results,
for example `8 queued, 2 failed`.

Double taps cannot start overlapping preparation. Back during preparation
requests cancellation, waits safely for the current verification step, and
does not prepare untouched items. Outbox rows already committed before
cancellation remain durable.

## Provider-Native Network Batching

The submission service must not deliver the selected scores with one HTTP
request per attempt. Durable status remains one outbox row per attempt, while
compatible rows share one provider request.

The driver and service batch boundary groups due Tachi entries by provider,
normalized request origin, request kind, and Batch Manual playtype. Tachi Batch
Manual has one `meta.playtype` per document, so mixed selections form separate
7K and 14K documents. Within each playtype, the driver combines score objects
into one `scores` array up to Tachi's 64 KiB payload bound and the service's
bounded worker batch size. Payload limits may create additional minimal chunks.
The request count is therefore the smallest valid number of batch requests,
never one request per score.

Before transport, every entry retains its independent attempt ID, ruleset
proof, payload validation, user-intent flag, retry counters, and durable state.
Batch claiming is atomic so another worker cannot deliver a subset
concurrently. The request reads the credential at send time; API keys never
enter payloads, outbox rows, receipts, diagnostics, or logs.

An immediate successful response settles every participating row together. A
deferred response stores the shared import ID on the group, and the worker
polls that shared import once rather than once per outbox row. Group completion
updates all rows and receipts transactionally.

Tachi may return fewer score IDs than input rows because equivalent scores
already exist, and it does not provide a reliable input-to-ID mapping. The
client never guesses that mapping. Successful rows receive durable receipts
without fabricated remote score IDs; later complete IR synchronization may
enrich those receipts.

If a response contains partial errors or otherwise cannot prove which inputs
succeeded, the client does not mark a guessed subset successful. The group
remains retryable with a bounded diagnostic. Retrying is safe because Tachi
score identity is deterministic and equivalent imports are idempotent.

Existing single-score automatic and manual submission remains supported. A
single compatible due row is simply a batch of one, using the same worker and
driver path.

## Live State and Completion

The page observes attempt-status revisions while visible. Durable enqueue,
active submit, deferred polling, failure, and receipt completion trigger a
bounded candidate refresh while preserving scroll offset and intersecting the
selection. Successfully uploaded rows do not remain as stale selectable cards.

The upload footer describes local preparation and durable queue acceptance;
it does not claim remote completion synchronously. Worker status and any later
failure remain visible through the existing Tasks and Records state surfaces,
and a failed attempt returns to the IR Uploads page as `Retry`.

## Failure and Concurrency Rules

- Provider configuration is rechecked at batch start and credential lookup is
  repeated at network request time.
- Provider-disabled and missing-key states offer the direct `Open IR Settings`
  action.
- Candidate reads, bulk chart hydration, selection refresh, and batch outcomes
  have explicit success/failure results; storage failure is never an empty
  success.
- Missing or changed charts cannot be uploaded from stale replay display data.
- Active, blocked, queued, and uploaded rows cannot be manually re-enqueued.
- Recycled views cannot retain another row's image, checkbox, or callback.
- Batch claim, shared deferred state, delivery completion, and receipt writes
  are atomic at the group boundary.
- Cancellation cannot erase already durable work or queue items that were not
  yet prepared.
- Diagnostics are sanitized and bounded, and credential values are redacted.

## Testing and Verification

Focused model and repository tests cover exact `Eligible` and `Failed`
filtering, multiple attempts for one chart, every excluded semantic state,
receipt scoping, corrupt provenance, bounded scans, missing chart metadata,
bulk path hydration, and explicit storage failures.

View and scene tests cover row content, jacket reuse, Select All/Clear,
selection counts, recycled-checkbox safety, refresh intersection, disabled
configuration, direct IR-settings navigation, empty/error states, progress,
cancellation, and final summaries.

Batch preparation tests cover continuation after local verification failures,
atomic enqueue/retry behavior, concurrent state changes, duplicate protection,
and accurate per-attempt outcomes.

Driver, response-parser, repository, and service tests cover multi-score Tachi
documents, proof validation, 7K/14K grouping, 64 KiB chunking, minimum request
counts, one POST for a compatible multi-score group, one poll for a shared
deferred import, atomic completion, duplicate imports, ambiguous partial
responses, cancellation, and compatibility with single-score automatic and
manual work.

Final verification runs the focused CTest targets, the complete configured
CTest suite, `git diff --check`, and
`cmake --build cmake-build-debug --target main -j 6`. No Firebase deployment is
part of this feature verification.

## Out of Scope

- Course, Auto Play, practice, modified, assisted, legacy-unverified, or
  otherwise ineligible score submission.
- Remote-only score rows, replay fabrication, or relaxed fingerprint checks.
- Waiting synchronously on the page for every remote import to complete.
- Guessing Tachi score-ID mappings for batch inputs.
- Changing ranking, reconciliation, or remote score projection behavior.
- Adding another IR provider as part of this page.
