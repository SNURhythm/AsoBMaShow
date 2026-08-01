# Durable IR Receipts and Remote Record Import Design

## Goal

Make the Records modal show the complete live Bokutachi submission state for
each eligible saved result and retain a green uploaded marker after the
temporary outbox row is purged. Add a manual Settings safety action that
rebuilds and removes durable local receipts from the user's complete remote BMS
score collection. The same two remote responses also restore score history from
other BMS clients as score-only Records entries and chart bests.

The outbox remains durable work state for delivery and crash recovery. A new
receipt is durable evidence that the equivalent score is represented on the
configured IR server. API keys remain runtime-only and are never copied into
outbox, receipt, or imported-score rows. A remote record is not represented as
a replay because Tachi does not provide input events or AsoBMaShow provenance.

## Source of Truth

The replay database owns delivery, receipt, and imported remote-record state:

- `ir_outbox` says whether an upload is absent, queued, active, awaiting a
  remote import, blocked, failed, or recently completed.
- `ir_submission_receipts` says whether a saved result is durably known to be
  represented on a provider and normalized server origin.
- `ir_remote_scores` is the validated, field-level mirror of complete remote
  primary-chart score snapshots. It contains score history but no synthetic
  replay events.
- `IrSubmissionService` status revisions add the current in-memory request
  activity without replacing either persistent source.

A single pure record-state resolver combines eligibility, receipt presence,
outbox state, and active request kind into the semantic state consumed by the
Records list. Views do not infer state from colors, local button flags, or a
single `irUploadPending` boolean.

Receipt presence takes precedence over stale outbox presentation. The outbox
still remains authoritative for unfinished network work until reconciliation
or normal delivery closes that work.

The canonical local `scores` table continues to contain locally produced score
projections only. A score-history projection merges validated `ir_remote_scores`
into chart best-score and clear-lamp caches. This lets imported IR records
restore chart progress without fabricating required local metrics or provenance.

## Durable Receipt and Remote Score Data

The profile replay database gains an `ir_submission_receipts` table. Each row
is scoped by:

- provider ID;
- normalized server origin;
- replay ID and canonical attempt ID;
- chart MD5 and SHA-256;
- optional remote user, chart, and score IDs;
- confirmation source and confirmation time;
- whether the receipt has ever been observed in a complete remote snapshot.

The replay foreign key cascades on replay deletion. Provider, origin, and
replay identity are unique together. A remote score ID is not unique because
Tachi deliberately deduplicates BMS scores by user, chart, EX score, and lamp;
multiple equivalent local attempts may therefore be represented by one remote
score.

No payload JSON, API key, authorization header, credential fingerprint, or
unbounded server diagnostic is stored in a receipt. Receipts for another server
origin remain isolated and do not produce markers under the active origin.
Changing or removing the active provider credential invalidates the active
receipt view so a different Bokutachi account cannot inherit another account's
checkmarks; the manual reconciliation can repopulate it.

Replacing or removing the Bokutachi credential transactionally clears the
active origin's imported-score mirror and receipt evidence. This prevents a
newly configured account from seeing the previous account's records. Locally
played scores and replays are unaffected, and the new account can be restored
with `Sync IR records`.

The same profile replay database gains an `ir_remote_scores` table keyed by
provider, normalized origin, and remote score ID. Required fields include the
remote user, game, chart ID, primary-chart hashes, EX score, lamp, source
service, and insertion time. The following remain nullable and retain their
availability explicitly:

- achieved time;
- PGreat, Great, Good, Bad, and Poor judgements;
- fast, slow, max combo, BP, and final gauge;
- gauge history;
- early/late judgement counts;
- random, gauge, input-device, and client metadata.

Chart title, artist, and note count come from the chart and song documents in
the same validated response. Gauge history and strings have independent bounds.
Raw response JSON is never persisted. Missing remote values remain null and are
never converted to zero. A reconciliation generation marker supports replacing
the complete imported snapshot only after both playtype responses validate.

## Atomic Submission Completion

Tachi import parsing retains the single returned `scoreID` and import `userID`
when present. `DeliveryOutcome` carries these bounded identifiers back to the
submission service without exposing the API key.

When a delivery succeeds, one replay-database transaction:

1. verifies and advances the claimed outbox row to `Succeeded`;
2. inserts or updates the receipt for the same attempt, provider, and request
   origin;
3. records the remote identifiers when Tachi supplied them.

If receipt persistence fails, the transaction rolls back and the outbox is not
reported as successful. A retry is safe because Tachi score identity is
deterministic.

Tachi may return an Import Document containing no score IDs and no errors when
an equivalent score already exists. This is treated as an idempotent success,
not a malformed response. It creates a receipt without a remote score ID; the
manual reconciliation may enrich it later. An empty score-ID list with import
errors remains a rejection.

Existing retained `Succeeded` outbox rows may still display as uploaded during
the compatibility window. The reconciliation action backfills durable receipts
for remotely matched rows. Already-purged historical success is recovered only
from reconciliation; no local code fabricates remote evidence.

## Records State, Imported Rows, and Icons

`ReplaySummary` exposes a semantic IR record state rather than the binary
`irUploadPending` marker. The compact badge keeps the literal `IR` label in the
normal UI font and renders its state glyph from FontAwesome Solid:

| State | Icon | Meaning and action |
| --- | --- | --- |
| Eligible | cloud upload | Not represented; tap to queue |
| Queued | clock | Waiting for the worker |
| Uploading | rotating arrows | POST is active |
| Awaiting remote | hourglass | Tachi import is being polled |
| Blocked | key | Provider configuration is required |
| Failed | warning triangle | Tap to retry |
| Uploaded | check | Durable receipt or retained successful outbox |

Queued and active badges are not duplicate upload actions. Tapping a blocked,
active, or uploaded badge may show concise status feedback but cannot create a
second outbox row. Recycled list rows replace their bound identity, state, icon,
color, enabled state, and callback on every bind.

While the Records modal is open, `MainMenuScene` observes Bokutachi attempt
status revisions. A revision change requeries affected summaries and rebinds
the virtualized list while preserving selection and scroll position. The
successful database transaction therefore changes the badge to the green
checkmark without closing the modal. A manual reconciliation completion
refreshes the entire visible Records result set because it may affect many
rows.

The Records list model becomes a tagged record summary with either a local
replay identity or an imported remote-score identity. Imported rows:

- always show the uploaded `IR` checkmark;
- show their remote achieved time, falling back to insertion time when the
  achieved time is absent;
- cannot be selected for replay playback, ghost use, or G-Battle;
- can open a partial result scene;
- are not eligible for submission, retry, or outbox creation.

When a remote score is linked to an equivalent eligible local replay receipt,
the standalone imported row is suppressed for that chart because the local
row has richer replay and result data. Tachi may map multiple equivalent local
attempts to the same remote score; all matching local rows keep their receipts,
and only one redundant standalone remote row is suppressed.

Imported score history contributes to the chart list's best EX score and clear
lamp through the merged score-history projection. Unknown optional metrics do
not participate in comparisons that require them. A later local play remains a
normal local score and replay; synchronization never rewrites it as remote.

Tachi BMS scores do not identify AsoBMaShow's selected long-note interpretation.
The projection therefore treats a remote score as an IR-wide chart-hash record
for score and lamp display across local long-note modes, visibly retaining its
IR source. It does not fabricate a local long-note mode or use the imported row
for ruleset-specific comparisons.

## Partial Result Scene Recall

Result presentation is separated from the assumption that every historical
result has a `ReplayData` and complete `RhythmState`. A shared result
presentation model contains optional card payloads. The existing local replay
builder fills the same complete payloads it does today, while a remote-result
builder fills only values stored in `ir_remote_scores` plus deterministic
display values such as rate and grade from EX score and note count.

The remote result scene uses explicit card requirements:

- title, artist, playtype, EX score, rate, grade, and lamp appear when their
  required chart and score fields are valid;
- the judgement card appears only with the complete five Tachi BMS judgement
  counts;
- combo/BP, final gauge, and gauge-history cards appear only when their remote
  metrics exist and validate;
- early/late details show only judgement rows for which both values exist;
- client, random, gauge, input device, service, and achieved-time metadata show
  only when supplied;
- timing histogram, lane offsets, section analysis, replay comparison, and any
  other raw-event card are omitted because Tachi does not provide their source
  data.

KPOOR is not reconstructed from BP for third-party scores because remote client
semantics may differ. BP remains labeled as BP when supplied. No absent value is
displayed as zero, and no placeholder card reserves layout space. The result
layout recomputes from the cards that are present, including in exported result
images.

Remote recall is read-only. It may expose Back, Rankings, and Export Photo when
their normal dependencies are available, plus an uploaded IR status. It never
exposes replay playback, IR upload/retry, or a verified-attempt context. Missing
or later-deleted remote records fail closed with a concise message and return to
Records.

## Reconciliation API Contract

Bokutachi reconciliation always makes exactly two score collection requests:

1. `GET /api/v1/users/me/games/bms-7k/scores/all`
2. `GET /api/v1/users/me/games/bms-14k/scores/all`

The configured per-profile API key authenticates `me`, avoiding a separate
identity lookup. Tachi exposes no game-group-wide equivalent, so two requests
are the smallest official complete BMS query. Both requests are made on every
run even if the local profile has records for only one playtype.

Tachi returns 404 when the authenticated user has no game profile for a
requested playtype. A valid Tachi error envelope that specifically identifies
the requested game as unplayed is accepted as a complete empty snapshot for
that playtype. An unknown 404, missing route, or malformed error remains a
failure. The other playtype request is still made, preserving the exactly-two
request contract.

The action is manual-only. Repeated taps join the in-flight operation, and a
short service-level cooldown prevents immediate repeat runs. There are no
automatic retries. The response body and all identifiers and collection sizes
are bounded before they enter reconciliation state.

The Tachi driver owns request construction and conversion of both successful
responses into a provider-neutral remote-score snapshot containing bounded
required and optional fields. The driver capability advertises whether full
receipt and record reconciliation is supported. Core Settings, view, and
repository code do not construct Tachi URLs or interpret Tachi JSON.

## Matching Semantics

For each remote score, reconciliation maps its chart ID through the returned
chart documents and builds the BMS identity:

- chart MD5 and SHA-256;
- EX score;
- lamp.

Only valid primary-chart documents and valid BMS score data enter the snapshot.
The two playtype responses must match their requested games. Scores from any
client service may confirm a local result: the checkmark means that the
equivalent score is represented in the user's IR account, not specifically
that AsoBMaShow created the remote row.

Eligible local single-chart results are matched by the same chart hash, EX
score, and mapped lamp. Tachi does not include play time in BMS score identity,
so all equivalent local attempts may share one remote score and receive
receipts. Course, Auto Play, modified, legacy-unverified, unsupported-ruleset,
and otherwise ineligible results never gain receipts from reconciliation.

Exact stored remote score IDs are matched first. Identity matching then
backfills missing receipts and missing remote identifiers.

Every validated primary remote score is also imported as a score-only record,
regardless of client service or whether it matches a local replay. Receipt
matching controls standalone-row suppression but does not discard the remote
record from the durable mirror or chart-best projection.

## Atomic Reconciliation

The submission service serializes reconciliation with its normal worker so a
remote snapshot cannot race a score upload or outbox mutation. It fetches both
responses into bounded memory before opening the database transaction. No
receipt or outbox state changes after only one successful response.

After both requests validate, one replay-database transaction:

1. replaces the provider/origin `ir_remote_scores` mirror with the validated 7K
   and 14K snapshot by remote score ID;
2. inserts or confirms receipts for all eligible local results represented by
   the snapshot;
3. enriches receipts with remote user, chart, and score IDs;
4. closes non-active duplicate outbox work that is already represented;
5. removes stale receipts whose previously observed primary-chart score is no
   longer in the complete snapshot;
6. cleans compatible retained-success rows after their durable state is
   resolved.

A receipt observed in a prior complete snapshot is safe to remove when its
remote score disappears, even when its chart no longer appears because the
user deleted the last score on that chart. A receipt that has never appeared
in `/scores/all` may represent a non-primary chart alias, which the endpoint
omits. Such a receipt is preserved as ambiguous rather than deleted. The
operation reports added, confirmed, removed, and ambiguous counts.

Remote score-only records are an authoritative mirror of the two complete
primary-score responses, so remotely deleted rows are removed from
`ir_remote_scores` in the same transaction. Locally played scores and replays
are never deleted. Commit bumps the imported-record and receipt revisions so
open Records and chart best/clear caches reload together.

Any HTTP, authentication, cancellation, size, JSON, validation, storage, or
profile-generation failure leaves receipts, imported records, and outbox rows
unchanged. Diagnostics are bounded and credential-redacted.

## Settings Interaction

The Bokutachi Settings card gains a `Sync IR records` safety button with helper
text explaining that it reconciles upload markers and imports remote score
history. It is enabled only when the active driver supports reconciliation, the
replay database and submission service are available, and a Bokutachi API key
is configured.

The button reports idle, fetching 7K, fetching 14K, applying, succeeded, and
failed states. It is disabled while active and during the service cooldown.
Success shows receipt and record added/confirmed/removed/ambiguous counts,
refreshes chart best/clear caches, and causes an open Records modal to reload.
Failure explains that local receipts and imported records were left untouched.
Leaving the Settings scene or switching profiles cancels the UI observation;
generation checks prevent a late operation from mutating another profile.

## Testing

Focused regression coverage includes:

- replay schema creation and migration create the exact receipt and remote-score
  tables and reject malformed partial schemas;
- successful delivery updates the outbox and receipt atomically and preserves
  the returned remote score ID;
- duplicate/no-error Tachi imports are idempotent successes while errored empty
  imports still fail;
- receipt persistence failure cannot publish a successful outbox state;
- semantic record-state resolution covers every outbox, request, eligibility,
  and receipt precedence case;
- virtualized rows replace FontAwesome icon, color, action, and identity on
  reuse;
- submission status revisions update an open Records modal and success changes
  to a checkmark without reopening it;
- reconciliation always performs one 7K and one 14K request, coalesces repeated
  taps, observes cooldown, and never retries automatically;
- either failed, malformed, oversized, or cancelled response causes zero local
  mutations;
- exact score IDs and chart-hash/score/lamp identities add and enrich receipts;
- equivalent local attempts may share a remote score receipt;
- ineligible local attempts are never marked;
- complete validated remote scores import idempotently by remote score ID and
  missing optional metrics remain unavailable rather than zero;
- imported records update merged chart score/clear caches without changing the
  canonical local score table;
- remotely deleted imported rows disappear while local scores and replays are
  untouched;
- Records merges local replay and remote score summaries, suppresses linked
  duplicates, and disables replay-only actions for remote rows;
- remote result recall includes every supported supplied card, omits cards with
  missing dependencies, never invents KPOOR, and exports the partial layout;
- local result recall retains its complete existing layout and behavior after
  adopting the optional presentation model;
- remotely deleted, previously observed receipts are removed, while
  never-observed non-primary ambiguity is preserved;
- reconciliation and delivery are serialized across cancellation and profile
  changes;
- receipt and outbox rows contain no API key.

Verification includes focused Tachi parser/driver, submission service,
repository, score-history projection, partial result presentation, Settings
model, record-state, and virtualized view tests; the full configured CTest
suite; `git diff --check`; and the desktop `main` build.

## Out of Scope

- Course score submission or course receipt badges.
- Automatic or periodic server reconciliation.
- More than the two official BMS score-collection requests per manual run.
- Treating non-primary-chart omission as proof of remote deletion.
- Persisting raw remote score collections or API credentials.
- Fabricating replay events, verified provenance, missing optional metrics, or
  KPOOR for imported records.
- Uploading an imported remote record back to the same or another IR provider.
- Adding a provider-specific reconciliation path outside the modular IR driver
  and submission-service architecture.
