# Bokutachi IR Integration Design

## Goal

Add durable Internet Ranking (IR) submission and chart-ranking reads to
AsoBMaShow without making gameplay, result persistence, chart selection, or
the result scene depend on Tachi-specific protocol details. The first provider
is Bokutachi through Tachi's Direct Manual and native BMS ranking endpoints,
but the internal models, driver boundary, queue, services, and UI must support
additional read-only or read/write IR providers without restructuring the
result flow.

## Approved Requirements

- Use a canonical, provider-neutral submission model and modular provider
  drivers.
- Give each driver explicit read-only and operation capability flags so a
  provider can expose rankings without supporting score submission.
- Implement Bokutachi with Tachi Direct Manual and Batch Manual JSON.
- Fetch Bokutachi personal-best rankings for a chart and expose them from both
  chart selection and the result scene.
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
`result_persistence::ChartResultAttempt`, plus on-demand Bokutachi ranking
reads for selected or completed BMS charts. The existing policy already
excludes autoplay, practice, replay playback, and course playback from the
result persistence path; it does not prevent a supported chart's remote
ranking from being viewed.

The Tachi driver supports the documented `bms:7K` and `bms:14K` playtypes.
Other key modes remain valid local results but are reported as unsupported for
this provider and are not placed into an automatic retry loop.

OAuth, account creation, browser login, course submission, course rankings,
server-side changes, platform background-execution entitlements, and arbitrary
third-party plugin loading are out of scope. LR2IR network or archive parsing
is explicitly deferred because the live service is unavailable; the driver
contract is prepared for a future read-only, archive-backed implementation.
The worker runs while the app is active and resumes durable work on startup,
foregrounding, and profile activation.

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
- `MainMenuScene` already has a right action rail and an `OverlayPortal` for
  modal UI; `ResultScene` already has a named `resultActions` row and a
  full-screen root where the same modal component can be attached.

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
        +-- TachiDriver
        +-- future provider drivers

selected/completed chart
        |
        v
canonical IrChartQuery
        |
        v
IrRankingService (memory cache)
        |
        v
IrDriver::fetchChartRanking
        |
        v
IrRankingModal
```

Provider-specific JSON and HTTP interpretation stay in the provider driver.
The result flow sees canonical submissions and generic queue status only. The
chart-selection and result scenes see canonical ranking models and modal
state, never Tachi response objects.

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
    IrRankingModels.h/.cpp
    IrRankingService.h/.cpp
    IrRankingModal.h/.cpp
    IrCredentialStore.h/.cpp
    IrHttpClient.h/.cpp
    tachi/
        TachiDriver.h/.cpp
        TachiBatchManual.h/.cpp
        TachiResponseParser.h/.cpp
        TachiRankingParser.h/.cpp
```

The exact private file split may be adjusted during implementation to keep
translation units cohesive. The public boundaries remain the canonical
models, driver interface, credential store, submission service, ranking
service, and ranking modal.

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

## Canonical Ranking Models

Ranking reads use a separate provider-neutral query and result model:

```cpp
struct IrChartQuery {
  int keyMode = 0;
  std::string chartMd5;
  std::string chartSha256;
  int totalNotes = 0;
};

struct IrChartRankingEntry {
  int rank = 0;
  std::string playerName;
  int score = 0;
  int maxScore = 0;
  int clearType = kClearTypeFailedRank;
  std::optional<int> badPoints;
  std::optional<int> maxCombo;
  std::optional<std::int64_t> achievedAtUnixMillis;
  bool currentUser = false;
};

struct IrChartRanking {
  std::string providerId;
  IrChartQuery chart;
  std::vector<IrChartRankingEntry> entries;
  std::int64_t fetchedAtUnixMillis = 0;
};
```

The query is built from the selected chart record or completed result. It is
immutable for one request. `rank` is the one-based provider rank assigned by
the driver while normalizing the response. The ranking service and UI preserve
the driver's order and never apply provider-specific sorting themselves.
Missing optional provider fields remain absent rather than being presented as
zero.

Ranking models contain no API key, Authorization header, raw response body,
provider URL, or mutable UI object. The local PB or current play is a separate
comparison model and is never inserted into `entries`.

## Driver Boundary

Each provider has a stable identifier, declares its supported operations, and
implements only those operations:

```cpp
struct IrDriverCapabilities {
  bool readOnly = false;
  bool chartRankings = false;
  bool scoreSubmission = false;
  bool deferredSubmission = false;
};

class IrDriver {
public:
  virtual ~IrDriver() = default;
  virtual std::string_view providerId() const noexcept = 0;
  virtual IrDriverCapabilities capabilities() const noexcept = 0;
  virtual BuildDraftOutcome buildDraft(const IrSubmission &) const;
  virtual DeliveryOutcome submit(const IrOutboxEntry &,
                                 const IrProviderRuntimeConfig &,
                                 IrHttpClient &,
                                 std::stop_token) const;
  virtual DeliveryOutcome poll(const IrOutboxEntry &,
                               const IrProviderRuntimeConfig &,
                               IrHttpClient &,
                               std::stop_token) const;
  virtual ChartRankingOutcome fetchChartRanking(
      const IrChartQuery &,
      const IrProviderRuntimeConfig &,
      IrHttpClient &,
      std::stop_token) const;
};
```

The base implementations return a typed `unsupported operation` outcome.
Driver registration rejects contradictory declarations:

- `readOnly` requires `scoreSubmission == false` and
  `deferredSubmission == false`.
- `deferredSubmission` requires `scoreSubmission == true`.
- A capability set must expose at least one operation.

The submission service also checks capabilities before asking for a draft or
calling submit/poll, so a read-only driver cannot create or deliver an outbox
row even if profile data is malformed. The ranking service calls only drivers
with `chartRankings == true`.

The Bokutachi driver sets `readOnly` to false and all three operation flags to
true. No LR2IR driver is registered in this implementation. A future
archive-backed LR2IR driver will set `readOnly` and `chartRankings` to true,
leave both submission flags false, and implement ranking reads without
submission stubs.

`BuildDraftOutcome` distinguishes a valid serialized payload from an
unsupported play or invalid canonical input. Unsupported input is a local,
user-readable result, not a network failure and not a retryable queue row.

`DeliveryOutcome` is provider-neutral and distinguishes success, deferred
remote work, transient failure, blocked configuration/authentication, and
permanent rejection. Technical response bodies remain inside the driver and
are reduced to bounded, sanitized diagnostics before reaching the queue.

`ChartRankingOutcome` distinguishes success, chart not found, authentication
required, transient failure, unsupported chart, and malformed or oversized
provider data. It returns only normalized entries and bounded diagnostics.

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

Settings presentation is capability-aware. Auto Submit, submission status,
and outbox actions are never shown for a read-only driver. Provider enablement
still controls ranking reads, and future drivers may define non-secret
read-specific configuration without pretending to support submission.

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
  chart_md5 TEXT,
  chart_sha256 TEXT NOT NULL,
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
`chart_md5` and `chart_sha256` repeat non-secret canonical identity needed for
provider-neutral status lookup and precise ranking-cache invalidation without
parsing provider JSON.

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

### Chart rankings

For a chart with a valid lowercase SHA-256 digest, the Bokutachi driver reads
the native Tachi BMS personal-best leaderboard. It first resolves the chart,
then obtains the authenticated numeric user ID before fetching the first
bounded ranking page:

```text
POST <server-origin>/api/v1/games/bms-{7k|14k}/charts/resolve
{"identifier":"<sha256>","matchType":"bmsChartHash"}

GET <server-origin>/api/v1/status

GET <server-origin>/api/v1/games/bms-{7k|14k}/charts/<chartID>/pbs?startRanking=<rank>
Authorization: Bearer <current-api-key>
```

The request does not follow redirects. HTTP 404 becomes the normal `chart not
found` empty state. HTTP 401/403 becomes `authentication required`; transport,
408, 429, and 5xx failures are transient. Other invalid responses become
bounded, sanitized errors and are not cached.

The resolved document must match the requested game, SHA-256, and note count.
The driver requests the first 100-row native PB page at rank 1. When `outOf`
shows more rows, the result includes a credential-free opaque continuation
token bound to the chart hash, chart ID, authenticated user ID, stable count,
loaded position, and previous rank. Each continuation performs exactly one PB
request and never repeats chart resolution or identity lookup. The service
appends pages only as the virtualized list approaches its final ten rows, so
popular charts do not delay the initial modal.

- PB `userID` maps through the page's user documents; the ID returned by
  `/api/v1/status` marks the current user. Names must be valid UTF-8 with 1
  through 64 Unicode code points and no control characters.
- Native `scoreData.score` is validated against `totalNotes * 2`.
- Native BMS lamp strings map to AsoBMaShow's canonical clear rank.
- `scoreData.optional.bp`, `maxCombo`, and `timeAchieved` populate BP, max
  combo, and achievement time when present and valid.
- Native optional `epg/lpg/egr/lgr` populate judgement detail only when all
  four exist and reproduce the stored EX score. Missing, partial, or
  inconsistent timing evidence is displayed as unavailable without rejecting
  the otherwise valid historical PB.
- Invalid rows fail the whole response instead of shifting ranks in a
  partially displayed remote list.

Each ranking page accepts at most 8 MiB and 100 entries. An `outOf` value above
20,000 or a response above the byte cap produces an oversized-response error.
Changed totals, malformed tokens, duplicate stable user IDs, regressing rows,
or early termination stop pagination while retaining the already loaded list.
Because Tachi accepts a rank rather than a row offset, one tie group larger
than the 100-row limit cannot be exhaustively enumerated; the same count check
keeps the verified prefix visible and reports the remaining page unavailable.

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

## Ranking Read Service

`ApplicationContext` owns one `IrRankingService` beside the submission
service. Ranking reads are on-demand, non-durable work and never enter
`ir_outbox`. The service owns one `std::jthread`, a condition variable, at most
one latest pending or active request, a per-request stop source,
request-generation counter, and a mutex-protected in-memory cache.

Opening a ranking modal submits an immutable request containing the active
profile ID, provider ID, validated server origin, chart query, and local
comparison snapshot. The service returns a matching successful cache entry
immediately or starts a cancellable background request. Refresh bypasses and
replaces the cache entry, cancels an older generation, and queues the new
generation. Closing the modal cancels its active or pending request without
waiting on the UI thread.

A successful ranking with a continuation token remains visible while the
service fetches one next page. Success appends immutable rows and replaces the
cached value. Page failure leaves the rows visible, exposes Refresh, and
blocks automatic retry so a near-end viewport cannot create a request loop.

Successful responses are cached for five minutes using a monotonic clock. A
cache key contains profile ID, provider ID, server origin, key mode, and chart
SHA-256 plus total note count. The API key is never part of the key or cached
value. Failed, authentication-required, unsupported, and chart-not-found
outcomes are not cached.

Every completion carries its request generation and full cache key. The UI
accepts it only if the modal is still open for that generation and chart. This
prevents a slow response from an earlier chart, profile, server, or key from
replacing current content.

Profile switching and application shutdown request cancellation and join the
active read before replacing profile-bound configuration or destroying the
service. Server URL changes, key replacement/removal, and provider disablement
cancel the active request and clear affected cache entries. When a score
submission succeeds, its profile, provider, request origin, and chart identity
invalidate the matching ranking cache entry so the next modal opening reads
the updated remote state.

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

These submission-specific controls are shown because Bokutachi is read/write.
A future read-only provider omits Auto Submit, queue counts, Retry All, and
discard controls while retaining its enablement and read-specific settings.

Each settings action writes exactly one store atomically before reconfiguring
the service: ordinary controls update `settings.json`, while Replace Key and
Remove Key update only `ir-credentials.json`. If that write fails, the UI and
effective runtime configuration retain the previous value and display a
sanitized error. No action attempts a transaction across the two files.

### Chart selection

The existing right action rail gains a `Rankings` button. It is enabled when
Bokutachi is enabled and the selected BMS chart has a supported key mode,
positive note count, and valid SHA-256 identity. Local file availability is
not required. A missing key does not disable the button; opening the modal
presents the authentication-required state and points the user to IR settings.

Clicking the button snapshots the selected chart and current local PB before
opening the modal. Ranking reads never start merely because selection moves,
so fast chart navigation cannot generate a stream of network requests. The
modal blocks list input until it is closed, making the chart snapshot stable.

### Ranking modal

Both entry points use the same `IrRankingModal`. It is a safe-area-aware,
full-screen overlay with a large centered panel rather than another permanent
column in either scene. Back/Escape, the platform back action, clicking the
scrim, or the explicit Close button cancels the active request and closes it.

The modal contains:

- Chart title and `Bokutachi Ranking` header.
- Last successful fetch time, Refresh, and Close controls.
- A comparison card above the leaderboard. Chart selection supplies the local
  PB; the result scene supplies `This Play`. The comparison is never assigned
  a remote rank or inserted into the server entries.
- A virtualized list with rank, player, EX score and rate, lamp, BP, and max
  combo.
- On-demand page loading near the final ten rows, retaining scroll position
  when immutable page snapshots are appended.
- A highlighted authenticated-user row labeled `You`.

On compact widths, each row keeps rank/player, EX rate, and lamp visible.
Selecting the row expands its BP, combo, and achievement time. The wide layout
shows those fields as columns without expansion.

The modal has explicit loading, empty/chart-not-found,
authentication-required, offline/transient failure, malformed/oversized
response, and retry presentations. Refresh bypasses the five-minute cache. No
modal state claims a local comparison has an official remote rank.

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

The normal result action row also gains `Rankings`. It opens the shared modal
for the completed chart with the current play as its separate comparison card.
It performs no implicit submission and is available independently of the
auto-submit toggle whenever Bokutachi ranking reads are enabled for the chart.

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
- Ranking requests send chart identity only; the local comparison score is not
  uploaded by opening the modal.
- Remote player names and scores live only in the bounded five-minute memory
  cache and active modal. They are not written to SQLite, settings, archives,
  diagnostics, or logs.
- Ranking cache keys and values contain no API key or raw response body.

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

Ranking read failures are scene-local and non-fatal. They never change outbox
state, block chart selection, block leaving the result screen, or affect local
score persistence. Request-generation checks turn late or cancelled
completions into no-ops.

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

### Driver capabilities and ranking mapping

- Accept the Bokutachi capability set and reject read-only/submission and
  deferred-without-submission contradictions.
- Prove a registered read-only fake driver cannot build a draft, create an
  outbox row, or receive submit/poll calls while it can serve rankings.
- Build chart queries from selected and completed charts and reject invalid
  key modes, hashes, and note counts.
- Parse Bokutachi ranking success, empty user name as `You`, every clear index,
  EX score/rate, BP, combo, timestamp, chart-not-found, authentication,
  malformed rows, oversized bodies, and too many entries.
- Reconstruct Tachi's BMS PB ordering and competition ranks deterministically,
  and fail the whole result when any row would make the remote rank sequence
  ambiguous.

### Repository and migration

- Create and migrate the outbox schema in `replays.db`.
- Prove replay, pending local score, and automatic IR draft commit or roll back
  together.
- Prove only the combined pending-score acknowledgement and IR activation
  transaction makes an automatic row claimable, including crash recovery
  after score projection.
- Prove `(provider_id, attempt_id)` idempotency.
- Retain canonical chart hashes needed for status lookup and ranking-cache
  invalidation without parsing provider payload JSON.
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

### Ranking service

- Use a fake monotonic clock, fake credential lookup, fake driver, and
  controllable HTTP completion.
- Verify fetch-on-open, five-minute successful cache reuse, Refresh bypass,
  and no caching of errors or chart-not-found outcomes.
- Verify cache separation by profile, provider, origin, key mode, chart, and
  note count; the API key must not appear in a key or value.
- Verify close, profile switch, provider disable, origin change, key change,
  and shutdown cancellation discard late generations.
- Verify successful submission invalidates only the matching
  profile/provider/origin/chart entry.

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
- Verify the chart-selection and result `Rankings` buttons open the same modal
  with local PB and `This Play` comparison models respectively.
- Verify comparison rows never receive a remote rank, the `You` row is
  highlighted, Refresh and Retry work, compact rows expand, and large lists
  remain virtualized.
- Verify loading, empty, authentication, transient, malformed, and oversized
  presentations without blocking either scene.
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
- Chart selection and the result scene can open the same Bokutachi ranking
  modal without prefetching during chart navigation.
- Rankings reconstruct Bokutachi's provider ordering, keep the local
  comparison outside that order, and refresh after the five-minute cache
  expires or the user requests it.
- A read-only driver cannot create or deliver an outbox row.
- No API key is present in SQLite, profile exports, provider payloads,
  ranking caches, diagnostics, or logs.
- Tachi-specific submission and ranking code remains behind the driver
  boundary, and adding a later archive-backed read-only provider does not
  require changes to gameplay result capture or modal presentation.

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
- Tachi native chart resolve and PB routes:
  <https://github.com/zkldi/Tachi/blob/233bc992f74cd314c8ef9bc2730d714904838dfc/typescript/server/src/server/router/api/v1/games/router.ts>
- Tachi beatoraja conversion avoided by native ranking reads:
  <https://github.com/zkldi/Tachi/blob/233bc992f74cd314c8ef9bc2730d714904838dfc/typescript/server/src/server/router/ir/beatoraja/charts/_chartSHA256/convert-scores.ts>
- Tachi BMS PB ranking keys:
  <https://github.com/zkldi/Tachi/blob/233bc992f74cd314c8ef9bc2730d714904838dfc/typescript/server/src/game-implementations/games/_common.ts#L82-L89>
- Tachi competition-rank materialization:
  <https://github.com/zkldi/Tachi/blob/233bc992f74cd314c8ef9bc2730d714904838dfc/db/migrations/20260518160000_chart_leaderboard_materialized.sql>
- Deferred LR2IR read-only reference:
  <https://github.com/SayakaIsBaka/lr2ir-read-only/tree/bb53fe823bc286ef4691b2d94a48bbeef029f989>
