# Bokutachi IR Integration Design

## Goal

Add durable Internet Ranking (IR) submission to AsoBMaShow without making
gameplay, result persistence, or the result scene depend on Tachi-specific
protocol details. The first provider is Bokutachi through Tachi's Direct
Manual endpoint, but the internal model, driver boundary, queue, worker, and UI
must support additional IR providers without restructuring the result flow.

## Approved Requirements

- Use a canonical, provider-neutral submission model and modular provider
  drivers.
- Implement Bokutachi with Tachi Direct Manual and Batch Manual JSON.
- Store provider enablement, auto-submit choice, and API credentials per player
  profile.
- Support automatic submission, explicit initial submission, and manual retry.
- Persist pending work in a local SQLite outbox so offline operation and
  process crashes do not lose queued submissions.
- Treat Tachi HTTP 202 responses as incomplete. Persist the remote import
  identity and poll until the import completes or fails.
- Never copy an API key into an outbox row, provider payload, diagnostic, or
  log message.

## Scope

The first implementation covers completed, locally persisted, single-chart
BMS results that the existing result-capture policy makes available through
`result_persistence::ChartResultAttempt`. The existing policy already excludes
autoplay, practice, replay playback, and course playback from this result
persistence path.

The Tachi driver supports the documented `bms:7K` and `bms:14K` playtypes.
Other key modes remain valid local results but are reported as unsupported for
this provider and are not placed into an automatic retry loop.

OAuth, account creation, browser login, course submission, leaderboards,
server-side changes, platform background-execution entitlements, and arbitrary
third-party plugin loading are out of scope. The worker runs while the app is
active and resumes durable work on startup, foregrounding, and profile
activation.

## Existing Repository Foundations

The design builds on these current boundaries:

- `GamePlayScene` creates an immutable `ChartResultAttempt` at the end of a
  supported play.
- `ResultPersistenceCoordinator` stages the replay and pending score before
  projecting the score to `scores.db`.
- `ReplayRepository::StageChartResult` owns the transaction that makes an
  attempt and its pending local work durable in the profile's `replays.db`.
- `ScoreRepository` owns the final local score record and the stable attempt
  identity used for idempotent projection.
- `ApplicationContext` owns repositories and profile-bound services.
- `ProfileSessionCoordinator` pauses profile-sensitive work and rebinds the
  active score and replay databases.
- `AppSettingsStore` persists ordinary per-profile settings, while profile
  archives currently include `settings.json`, `scores.db`, and `replays.db`.

## Chosen Architecture

The data flow is:

```text
ChartResultAttempt
        |
        v
canonical IrSubmission
        |
        v
IrDriver::BuildOutboxDraft
        |
        v
ReplayRepository::StageChartResult
        |
        +-- replay rows
        +-- pending local score row
        +-- generic IR outbox row
        |
        v
IrSubmissionService worker
        |
        +-- TachiDirectManualDriver
        +-- future provider drivers
```

Provider-specific JSON and HTTP interpretation stay in the provider driver.
The result flow sees canonical submissions and generic queue status only.

### Outbox placement

The generic IR outbox is stored in the active profile's existing
`replays.db`. This is the only placement that lets the replay, pending local
score, and automatic IR row commit in the existing result-staging transaction.
It closes the crash window in which a local score could become durable but the
corresponding automatic submission had not yet been queued.

A separate `ir.db` was rejected for the first implementation because it would
require a cross-database commit or a reconciliation protocol, plus another
profile database lifecycle, archive component, migration path, and switch
binding. `scores.db` was rejected because a network delivery queue is not score
record ownership.

The table and its operations are provider-neutral even though they live in the
result persistence database. `ReplayRepository` does not construct Tachi JSON,
perform HTTP, read credentials, or classify provider responses.

## Source Organization

New IR code is grouped under `src/ir/`:

```text
src/ir/
    IrSubmission.h/.cpp
    IrDriver.h
    IrOutboxModels.h/.cpp
    IrSubmissionService.h/.cpp
    IrCredentialStore.h/.cpp
    IrHttpClient.h/.cpp
    tachi/
        TachiDirectManualDriver.h/.cpp
        TachiBatchManual.h/.cpp
        TachiResponseParser.h/.cpp
```

The exact private file split may be adjusted during implementation to keep
translation units cohesive. The public boundaries remain the canonical model,
driver interface, credential store, and submission service.

All new supported source files must be added to the applicable desktop and
mobile CMake targets. The iOS file-system-synchronized group discovers normal
new source files automatically; only platform-only or build-metadata files may
be added to its small `membershipExceptions` list.

## Canonical Submission Model

`IrSubmission` is an immutable snapshot of one completed local play. It
contains only provider-neutral data needed by drivers:

```cpp
struct IrSubmission {
  std::string attemptId;
  int keyMode = 0;
  std::string chartMd5;
  std::string chartSha256;
  int score = 0;
  int maxScore = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  int pGreat = 0;
  int great = 0;
  int good = 0;
  int bad = 0;
  int poor = 0;
  int kPoor = 0;
  int fast = 0;
  int slow = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::int64_t playedAtUnixMillis = 0;
  ScoreProvenance provenance;
};
```

The model is built from the already validated `ChartResultAttempt` and chart
metadata at result completion. `playedAtUnixMillis` is captured once at the
end of the play. Retries use the persisted provider payload and never replace
this time with a retry time.

`score` is AsoBMaShow's EX score (`PGreat * 2 + Great`), and `maxScore` is
`total notes * 2`. It is not a normalized display score. Drivers can therefore
pass it to providers whose BMS score metric is EX score without recomputing it.

The canonical model does not include a server URL, API key, HTTP headers,
Tachi names, JSON, or queue state. Drivers validate provider support before
producing an outbox draft.

## Driver Boundary

Each provider has a stable identifier and implements these conceptual
operations:

```cpp
class IrDriver {
public:
  virtual ~IrDriver() = default;
  virtual std::string_view providerId() const noexcept = 0;
  virtual BuildDraftOutcome buildDraft(const IrSubmission &) const = 0;
  virtual DeliveryOutcome submit(const IrOutboxEntry &,
                                 const IrProviderRuntimeConfig &,
                                 IrHttpClient &,
                                 std::stop_token) const = 0;
  virtual DeliveryOutcome poll(const IrOutboxEntry &,
                               const IrProviderRuntimeConfig &,
                               IrHttpClient &,
                               std::stop_token) const = 0;
};
```

`BuildDraftOutcome` distinguishes a valid serialized payload from an
unsupported play or invalid canonical input. Unsupported input is a local,
user-readable result, not a network failure and not a retryable queue row.

`DeliveryOutcome` is provider-neutral and distinguishes success, deferred
remote work, transient failure, blocked configuration/authentication, and
permanent rejection. Technical response bodies remain inside the driver and
are reduced to bounded, sanitized diagnostics before reaching the queue.

## Profile Settings and Credentials

Non-secret provider settings are added to `AppSettings` and remain in
`settings.json`:

```cpp
struct IrProviderSettings {
  bool enabled = false;
  bool autoSubmit = false;
  std::string serverUrl = "https://boku.tachi.ac";
};
```

The first settings entry is keyed by the stable provider ID `tachi`. The
settings schema is versioned and sanitized. Server URLs must be absolute HTTP
or HTTPS origins without embedded user information, query strings, or
fragments. HTTPS is the normal production choice; HTTP is allowed only for an
explicitly entered development or self-hosted instance and is labeled as
insecure in the UI.

The API key is stored separately in a versioned, atomically written
`ir-credentials.json` inside the profile directory. The credential store is
keyed by provider ID. It is not part of `AppSettings`, and the UI displays only
whether a key exists plus a masked editor value.

The credential file is device-local operational state:

- Profile export and import do not include it.
- Profile duplication does not copy it.
- A newly imported or duplicated profile starts with no IR credentials.
- Profile deletion removes it with the profile root.
- Replacing a key wakes the worker and makes blocked rows eligible again.
- Empty or missing credentials are a normal `authentication required` state,
  not profile corruption.

The first implementation uses the repository's atomic file-persistence model
rather than adding platform-specific Keychain, Keystore, Credential Manager,
or secret-service dependencies. File contents and key values must never be
logged.

## Durable Outbox

The replay database schema gains a generic table equivalent to:

```sql
CREATE TABLE ir_outbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  provider_id TEXT NOT NULL,
  attempt_id TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  state INTEGER NOT NULL,
  local_result_ready INTEGER NOT NULL DEFAULT 0,
  request_attempt_count INTEGER NOT NULL DEFAULT 0,
  consecutive_failure_count INTEGER NOT NULL DEFAULT 0,
  next_attempt_at_ms INTEGER,
  next_request_user_intent INTEGER NOT NULL DEFAULT 0,
  remote_job_id TEXT,
  remote_origin TEXT,
  last_error_code TEXT,
  last_error_message TEXT,
  created_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL,
  completed_at_ms INTEGER,
  UNIQUE(provider_id, attempt_id),
  CHECK ((remote_job_id IS NULL AND remote_origin IS NULL) OR
         (remote_job_id IS NOT NULL AND remote_origin IS NOT NULL))
);
```

All `_at_ms` columns contain Unix time in milliseconds supplied through the
repository clock. `local_result_ready` is zero while the corresponding score
is still in the local projection/recovery path. The worker only claims rows
whose readiness value is one. The row intentionally has no foreign key to the
replay: its stable payload must survive chart or replay cleanup until it
reaches a terminal state or the user explicitly discards it. The atomic stage
transaction and canonical `attempt_id` establish its original association.

`request_attempt_count` counts all POST and poll requests for diagnostics.
`consecutive_failure_count` drives transient-error backoff and resets after a
valid protocol response. An ongoing remote import uses the normal poll
interval rather than being counted as a transport failure.

`payload_json` is the fully serialized provider payload. It deliberately does
not contain the API key, Authorization header, server URL, response cookies,
or other credentials. Persisting the provider payload makes retries stable
across chart removal, metadata changes, and future mapping changes.

The row states are:

- `pending`: eligible for a new submission attempt when due.
- `uploading`: claimed by the current worker process.
- `awaiting_remote_result`: a submission returned HTTP 202 and must be polled.
- `blocked_configuration`: a key is missing or authentication/configuration
  must change before another automatic attempt.
- `failed_permanent`: the provider rejected the payload or returned a
  non-retryable protocol result.
- `succeeded`: the remote import completed and accepted the score.

On startup, a stale `uploading` row returns to `awaiting_remote_result` when it
has a `remote_job_id`; otherwise it returns to `pending`. This provides
at-least-once delivery after an app crash without POSTing a known deferred job
again. A POST that reached the server but whose response was lost may still be
repeated; the UI and diagnostics must not claim exactly-once network delivery.

Successful rows are retained for seven days for result-screen status and
diagnostics, then purged. Pending, deferred, blocked, and failed rows remain
until they succeed or the user explicitly discards them.

### Archive and duplication behavior

The IR outbox is device-local operational state and is not copied into a new
remote-submission identity. Profile export and local profile duplication use
their database snapshots as today, then clear `ir_outbox` in the snapshot
before packaging or finalizing the duplicate. The source profile is never
modified. This prevents imported or duplicated profiles from unexpectedly
replaying the same pending submissions against another credential.

## Atomic Automatic-Submission Flow

For a provider with `enabled && autoSubmit`:

1. `GamePlayScene` finishes replay recording and creates the existing
   `ChartResultAttempt`.
2. It builds an `IrSubmission` and asks registered drivers for outbox drafts.
   This step performs no network access and reads no API key.
3. `ResultPersistenceCoordinator::persist` receives the attempt plus zero or
   more generic outbox drafts.
4. `ReplayRepository::StageChartResult` inserts the replay, pending local
   score, and drafts in one transaction.
5. The existing coordinator projects the local score to `scores.db`.
6. In one `replays.db` transaction, a new repository operation acknowledges
   the pending local score and changes every matching IR row's
   `local_result_ready` value to one.
7. The service is notified and the worker may claim the ready IR row.

If local staging fails, no IR row exists. If the process stops after score
projection but before the combined acknowledgement-and-activation
transaction, the pending score remains. Startup recovery projects it
idempotently, then uses that same combined transaction to remove the pending
write and activate the outbox rows. Network delivery therefore cannot race
ahead of local result persistence, and a crash cannot strand an inactive row
after removing its recovery marker.

The uniqueness constraint makes repeated calls with the same attempt and
provider idempotent.

## Manual Submission and Retry

When the provider is enabled but auto-submit is off, no automatic row is
created. After the local result is durable, the result scene offers `Submit`.
That action builds and inserts the same provider payload with
`local_result_ready = 1` and `next_request_user_intent = 1`, then wakes the
worker.

`Retry` or `Retry All` performs these operations without changing payload
JSON:

- A transient, blocked, or permanently failed row returns to `pending` and is
  due immediately.
- The next new POST carries user intent because the user explicitly requested
  it.
- A row already awaiting a Tachi remote result is polled immediately and is
  not resubmitted.
- The request uses the current profile credential, not any value captured when
  the row was created.

`next_request_user_intent` is consumed and cleared when the next POST actually
begins. Later scheduled retries omit the header unless the user presses Retry
again. Poll requests never carry `X-User-Intent`.

Discard is a separate, explicitly labeled action. Disabling auto-submit stops
new automatic rows but does not delete or silently cancel existing work.
Disabling the provider pauses all of its delivery work without changing row
state; re-enabling it resumes eligible rows. A pending row uses the server
origin currently configured when its POST begins.

## Tachi Batch Manual Mapping

The Tachi driver emits a one-score Batch Manual document:

```json
{
  "meta": {
    "game": "bms",
    "playtype": "7K",
    "service": "AsoBMaShow"
  },
  "scores": [
    {
      "score": 1234,
      "lamp": "HARD CLEAR",
      "matchType": "bmsChartHash",
      "identifier": "<sha256-or-md5>",
      "timeAchieved": 1700000000000,
      "judgements": {
        "pgreat": 500,
        "great": 100,
        "good": 10,
        "bad": 2,
        "poor": 1
      },
      "optional": {
        "fast": 30,
        "slow": 40,
        "maxCombo": 550,
        "bp": 3,
        "gauge": 82.0
      }
    }
  ]
}
```

Mapping rules:

- Key mode 7 maps to playtype `7K`; key mode 14 maps to `14K`.
- `score` is the captured, validated EX score.
- SHA-256 is preferred for `identifier`; a valid MD5 is the fallback.
- `matchType` is `bmsChartHash`.
- `timeAchieved` is the captured Unix time in milliseconds.
- Clear ranks map to `FAILED`, `ASSIST CLEAR`, `EASY CLEAR`, `CLEAR`,
  `HARD CLEAR`, `EX HARD CLEAR`, and `FULL COMBO`.
- Both AsoBMaShow assisted-easy ranks map to Tachi `ASSIST CLEAR`.
- Tachi's BMS judgement set receives PGreat, Great, Good, Bad, and Poor.
  AsoBMaShow KPoor is omitted because Tachi does not define a KPoor judgement.
- `bp` is Bad plus Poor, matching Tachi's documented BMS metric. KPoor is not
  added to BP.
- Gauge is clamped to the documented 0 through 100 range.
- The driver validates score ranges, hashes, timestamp, non-negative counts,
  key mode, and JSON size before producing a draft.

The provider-neutral provenance remains available for later provider policy
and score metadata support. The first driver does not invent unsupported
Tachi fields.

## Tachi HTTP Protocol

The submit request is:

```text
POST <server-origin>/ir/direct-manual/import
Authorization: Bearer <current-api-key>
Content-Type: application/json
X-User-Intent: true   # explicit Submit/Retry only
```

Automatic background submissions omit `X-User-Intent` or set it to false.
Redirects are not followed for authenticated POST or polling requests. This
prevents forwarding the API key to another origin.

The HTTP abstraction returns the status code, bounded response body, selected
safe headers such as `Retry-After`, and a transport error category. It is
dependency-injected into drivers for deterministic testing. Desktop and
Android use the already linked libcurl with the repository trust-store setup;
iOS uses a focused `NSURLSession` implementation because the iOS target does
not link libcurl.

### Immediate response

A successful non-202 response is parsed as an Import Document. Because each
outbox payload contains one score, the driver treats an accepted score ID as
success. A completed response containing only converter errors becomes a
sanitized permanent failure. Mixed success and warnings is success with a
bounded diagnostic.

### Deferred response

HTTP 202 is not success. The driver requires a valid `importID`, persists it,
records the validated origin used for that POST in `remote_origin`, stores the
Tachi import ID as the generic `remote_job_id`, and moves the row to
`awaiting_remote_result`. It does not persist an API key or blindly trust the
response's `url` field.

Polling constructs this same-origin URL from the persisted request origin and
the validated import ID:

```text
GET <server-origin>/api/v1/imports/<importID>/poll-status
Authorization: Bearer <current-api-key>
```

`importStatus = ongoing` schedules another poll. `completed` parses the
returned Import Document using the same acceptance rules as an immediate
response. Changing the configured server affects new POSTs but never moves an
existing deferred import to another origin. A manual retry while awaiting
completion triggers a poll, never a second POST.

## Retry Classification

Automatic retries use `consecutive_failure_count` and a persisted capped
schedule equivalent to 10 seconds, 30 seconds, 2 minutes, 10 minutes, and 1
hour. The one-hour value remains the cap for later transient failures. A valid
longer `Retry-After` value may delay the next attempt further. An `ongoing`
poll response resets the failure count and schedules the next poll after 10
seconds.

Transient failures include:

- Offline, DNS, connection, TLS, and timeout failures.
- HTTP 408 and 429.
- HTTP 5xx responses.

Missing credentials and HTTP 401/403 enter `blocked_configuration` rather than
hammering the server. Saving a replacement key or choosing Retry makes them
due again.

Permanent failures include:

- Unsupported or malformed provider payloads that escaped preflight.
- Non-authentication HTTP 4xx responses unless explicitly classified as
  transient.
- Invalid or unsafe 202 response data.
- Completed imports that reject the only submitted score.
- Malformed successful response bodies.

All classifications are driver outcomes, so a future provider can use
different response rules without changing the worker.

## Worker and Concurrency Model

`IrSubmissionService` owns one `std::jthread`, a condition variable, the driver
registry, a thread-safe active-profile configuration snapshot, and a bounded
status cache for UI reads.

The worker:

1. Recovers stale `uploading` rows at startup.
2. Selects one due, locally ready pending or deferred row for an enabled
   provider in deterministic order.
3. Reads the current provider configuration and credential. Missing
   configuration moves a due row to
   `blocked_configuration` without consuming user intent.
4. Atomically claims the still-eligible row, increments its request-attempt
   count, and consumes `next_request_user_intent` only for a new POST.
5. Performs one cancellable network operation outside the SQLite transaction.
6. Persists the driver outcome and wakes for the next due item.

Only one row is in flight initially. This avoids provider bursts, makes result
status deterministic, and is adequate for one row per play. Concurrency can be
raised later without changing the driver contract.

Profile switching pauses the worker, requests cancellation of an in-flight
HTTP operation, waits for the worker to release profile-bound database work,
then lets `ProfileSessionCoordinator` rebind `ReplayRepository`. After the
target settings and credential state are loaded, the service receives a new
configuration snapshot and resumes. Rollback restores the previous snapshot.

Application shutdown stops and joins the worker before repositories are shut
down. Foregrounding and configuration changes wake it. No UI thread waits for
a normal upload or poll.

## UI Behavior

### IR settings

Add an `IR` settings tab for the active profile with:

- Provider enabled toggle.
- Auto Submit toggle.
- Server URL editor, prefilled with `https://boku.tachi.ac`.
- Masked API key editor with Replace and Remove actions.
- Pending, awaiting, blocked, and failed counts.
- `Retry All Now` and explicit discard controls.
- A short note that credentials are device-local and excluded from profile
  export.

Each settings action writes exactly one store atomically before reconfiguring
the service: ordinary controls update `settings.json`, while Replace Key and
Remove Key update only `ir-credentials.json`. If that write fails, the UI and
effective runtime configuration retain the previous value and display a
sanitized error. No action attempts a transaction across the two files.

### Result scene

For a supported normal result, show a compact provider-neutral IR status:

- `Not submitted` with `Submit` when auto-submit is off.
- `Queued`.
- `Submitting...`.
- `Waiting for Bokutachi...`.
- `Submitted`.
- `Authentication required` with `Retry` after the key is updated.
- `Failed` with `Retry` and a bounded user-readable reason.
- `Unsupported` with the local reason and no retry button.

The existing local result persistence decision remains higher priority. IR
controls do not appear as successful until the local result is durable, and IR
failure never blocks leaving the result screen because the outbox is durable.

The scene reads a thread-safe service status snapshot keyed by provider and
attempt ID. It does not query SQLite every frame and never receives raw HTTP
response bodies or credentials.

## Security and Privacy

- API keys exist only in the credential store, masked UI editor state, and the
  in-memory request header assembled immediately before sending.
- Queue rows, Batch Manual payloads, profile settings, profile exports,
  diagnostics, and logs contain no API key.
- HTTP diagnostics never include Authorization headers, full response bodies,
  or credential-bearing URLs.
- Authenticated requests do not follow redirects.
- Deferred polling uses a same-origin URL constructed from a validated import
  ID, preventing a response from redirecting credentials to another host.
- JSON and response bodies have explicit size limits.
- Server error text is bounded and sanitized before persistence or display.
- Provider payloads contain score and chart-hash data and therefore remain
  inside the profile database and credential-authorized submission flow.

## Error Handling

Application-facing IR boundaries are non-throwing. Construction, repository,
driver, HTTP, and parsing failures return typed outcomes. Exceptions from
third-party code are caught at the service boundary and converted to sanitized
transient or permanent outcomes as appropriate.

SQLite mutations use transactions and checked bindings. A malformed future
outbox schema fails profile/database readiness according to the existing
validated-open policy rather than being overwritten. Corrupt individual rows
are quarantined as permanent failures when they can be identified safely; a
database-level integrity failure remains fatal to profile activation.

IR startup recovery is warning-level. Existing local score/replay startup
readiness remains authoritative, and a network outage never prevents the app
from starting.

## Testing Strategy

### Canonical model and Tachi mapping

- Build canonical submissions from fixed result attempts.
- Verify 7K and 14K metadata, SHA-256 preference, MD5 fallback, every lamp,
  judgement mapping, KPoor omission, BP calculation, gauge clamping, and Unix
  millisecond timestamps.
- Reject unsupported key modes, invalid digests, invalid ranges, and oversized
  payloads.
- Parse immediate success, converter rejection, mixed warnings, malformed
  JSON, and 202 responses.

### Repository and migration

- Create and migrate the outbox schema in `replays.db`.
- Prove replay, pending local score, and automatic IR draft commit or roll back
  together.
- Prove only the combined pending-score acknowledgement and IR activation
  transaction makes an automatic row claimable, including crash recovery
  after score projection.
- Prove `(provider_id, attempt_id)` idempotency.
- Recover stale uploads, preserve deferred import IDs, and retain retry times.
- Preserve the origin that accepted a deferred import even when profile
  settings later select another server.
- Preserve active outbox rows when their source replay or chart is removed.
- Verify profile switching binds the correct queue.
- Verify profile export and duplication clear only the snapshot outbox and do
  not modify the source profile.

### Worker state machine

- Use fake clock, fake credential lookup, and fake HTTP transport.
- Cover offline recovery, timeout, 408, 429 with `Retry-After`, 5xx backoff,
  authentication blocking, credential replacement, permanent rejection, and
  seven-day success cleanup.
- Prove a 202 row polls until completion and is never POSTed again, including
  after restart and manual Retry.
- Prove profile switching and shutdown cancel or drain the worker without
  updating the wrong profile database.

### Credential hygiene

- Verify the API key is absent from every outbox column, serialized Batch
  Manual payload, settings JSON, profile archive, duplicate profile, persisted
  diagnostic, and captured log.
- Verify credential save/load/remove behavior and failure rollback.
- Verify authenticated redirects are rejected and polling remains same-origin.

### UI and integration

- Unit-test result-status and settings presentation models separately from
  network and SQLite.
- Verify Submit, Retry, Retry All, Remove Key, disable auto-submit, and discard
  actions.
- Build the desktop target and focused tests, then run the repository's iOS and
  Android build-only verification paths without deployment.

## Success Criteria

- A supported completed play is saved locally before it can be transmitted.
- With auto-submit enabled, the provider payload is durably queued in the same
  result-staging transaction.
- Closing or crashing the app while offline does not lose a queued submission.
- Restarting or replacing an API key resumes eligible work without rebuilding
  payloads.
- A Tachi 202 response cannot be mistaken for success and is polled to a
  terminal remote result.
- Manual retry uses the current credential and does not resubmit an already
  deferred import.
- No API key is present in SQLite, profile exports, provider payloads,
  diagnostics, or logs.
- Tachi-specific code remains behind the driver boundary, and adding another
  provider does not require changes to gameplay result capture.

## Protocol References

- Tachi Direct Manual:
  <https://docs.tachi.ac/codebase/batch-manual/direct-manual/>
- Tachi Batch Manual:
  <https://docs.tachi.ac/codebase/batch-manual/>
- Tachi import document polling:
  <https://docs.tachi.ac/api/routes/imports/>
- Tachi API authentication:
  <https://docs.tachi.ac/api/auth/>
- Tachi BMS 7K support:
  <https://docs.tachi.ac/game-support/games/bms-7K/>
- Tachi BMS 14K support:
  <https://docs.tachi.ac/game-support/games/bms-14K/>
