# Database Repository Refactor Design

## Goal

Refactor AsoBMaShow's SQLite access into concrete, domain-oriented
repositories while preserving runtime behavior, database compatibility, and
performance except for explicitly approved, regression-tested correctness or
data-safety fixes. SQLite remains the only persistence backend, so the design
does not introduce repository interfaces, generic repository templates, or
implementation prefixes such as `SQLite`.

The refactor covers the chart, score, replay, and music-playlist SQLite areas.
JSON-backed settings, input profiles, and practice presets are out of scope.

## Constraints

- Existing database filenames, schemas, schema versions, migrations, indexes,
  pragmas, and stored data remain compatible.
- Existing query ordering, filtering, pagination, recovery, cancellation,
  progress reporting, logging, and user-visible error behavior remain
  unchanged except for the chart fail-closed case below.
- Score, replay, and music retain their validated, fail-closed open paths.
  Apply the same preflight to chart persistence as an explicitly approved
  data-safety fix: corrupt or future-version chart database families are
  rejected before a read-write open, pragma, migration, journal checkpoint, or
  sidecar mutation. Supported chart databases retain WAL and
  `synchronous=NORMAL` after validation, including the current checkpoint-on-
  last-close policy.
- Other behavior changes are allowed only for a major, straightforward
  correctness or data-safety defect with a deterministic regression test. Keep
  each such fix isolated and call it out separately from mechanical refactoring.
- Current transaction and rollback boundaries remain intact unless an
  explicitly approved defect fix has a deterministic regression test.
- Current profile-switch activity guards and atomic connection replacement
  remain intact.
- Hot-path query counts, query plans, connection reuse, statement reuse,
  bounded caches, and concurrency must remain at least as efficient as the
  current implementation.
- New source files under `src` must be added to `membershipExceptions` in
  `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`.

## Chosen Approach

Perform a clean repository cutover in independently buildable slices. Rename
and reshape the existing helpers, update every internal caller, and remove the
old helper and singleton APIs rather than keeping a compatibility facade.

A rename-only change was rejected because it would preserve raw SQLite handles,
mixed responsibilities, and global dependency lookup. A compatibility facade
was rejected because it would temporarily create two competing APIs and make
connection ownership harder to reason about.

## Source Organization

All repository code is grouped under `src/repositories/`:

- `ChartRepository.*` owns chart-library persistence and domain queries.
- `ScoreRepository.*` owns score persistence, caches, and recovery.
- `ReplayRepository.*` owns replay persistence, result outbox state, and
  recovery.
- `MusicPlaylistRepository.*` owns playlists, stored tracks, queue snapshots,
  and player state.
- Focused private implementation files in the same folder split schema and
  migration code, query construction and row mapping, and serialization where
  a repository would otherwise become unwieldy.
- Shared SQLite RAII, validated-open, statement, binding, and transaction
  helpers used by multiple repositories also live under
  `src/repositories/`. They remain persistence details and are not application
  APIs.

`ChartLibraryScanner` and `DifficultyTableImporter` live outside
`src/repositories/` because filesystem scanning, archive parsing, network
loading, and format parsing are application workflows rather than persistence.
They depend on `ChartRepository`; the repository does not depend on them.

The split follows cohesive responsibilities rather than creating one file per
table or one generic CRUD abstraction.

## Ownership and Dependency Flow

`ApplicationContext` owns one concrete production instance of each repository:

- `ChartRepository`
- `ScoreRepository`
- `ReplayRepository`
- `MusicPlaylistRepository`

Scenes, services, startup initialization, and coordinators receive references
to those objects through the context or their constructors. The four
repositories do not expose `GetInstance()` and callers do not perform global
repository lookup.

Raw `sqlite3*` connections, statements, migrations, row mapping, and
transactions remain inside `src/repositories/`. Scenes and application
services neither include SQLite for repository access nor manage connection
cleanup.

### Chart sessions

The chart database currently relies on a retained main-menu connection plus
independent connections for background scans, imports, and settings work.
`ChartRepository` therefore exposes scoped, domain-level sessions rather than a
raw connection:

```cpp
auto session = chartRepository.OpenSession(&scoreRepository);
std::vector<ChartMetaRecord> records;
session->QueryChartMeta(query, records);
```

`ChartRepository::EnsureReady()` performs the full unchanged-family preflight
once before schema initialization. Each later session owns one cheaply opened,
trusted connection that verifies the ready schema version and WAL mode without
repeating the full database snapshot/integrity pass. The main menu retains one
session for paging and its existing bounded page cache. Background operations
open independent sessions so they do not introduce a new global chart lock.
Session methods expose chart-domain operations only.

Where chart queries need the active score database, `ScoreRepository` creates a
scoped score-query lease for a chart session. The lease retains the existing
attached-database SQL join and score-session/profile guard; it does not expose
the attached schema or connection to application code.

### Score and replay sessions

`ScoreRepository` and `ReplayRepository` retain their current mutex-protected,
long-lived profile connections. Profile binding validates and migrates a
candidate connection before atomically replacing the active connection. An
unfinished transaction still causes the connection to be discarded according
to current behavior.

### Music session

`MusicPlaylistRepository` retains one long-lived validated connection while the
music service is active. Repository calls continue to run under the existing
music-service lock, and existing in-memory playlist and queue caches remain in
the service.

## Repository Responsibilities

Repositories expose domain use cases rather than table-shaped CRUD.

### ChartRepository

- Initialize and validate the chart schema.
- Query, count, page, and locate chart metadata with existing filter and sort
  semantics.
- Persist chart metadata batches supplied by the scanner.
- Manage favorites and chart-folder entries.
- Persist and query difficulty tables, levels, courses, and course definitions.
- Preserve chart path normalization, archive metadata, and library revision
  behavior.

`ChartRepository` does not crawl filesystems, parse chart files or archives,
download difficulty tables, or parse remote JSON.

### ScoreRepository

- Initialize, validate, and migrate the active profile's score database.
- Save chart and course scores, including idempotent projected results.
- Load best scores, clear-rank caches, and score caches.
- Recover course score identities and expose recovery evidence.
- Preserve score revision and score-query lease behavior.

### ReplayRepository

- Initialize, validate, and migrate the active profile's replay database.
- Save and load chart and course replays.
- Stage, list, acknowledge, and record recovery attempts for pending chart
  score writes.
- List bounded replay summaries and hydrate full replay data.
- Recover course replay identities.

### MusicPlaylistRepository

- Initialize and validate music persistence and its chart-database attachment.
- Manage playlists and stored tracks.
- Persist now-playing tracks, selected playlist state, playback mode, and queue
  position.
- Query library and group tracks with the existing chart-selection rules.

## Application Services and Coordinators

`ChartLibraryScanner` owns filesystem and archive discovery, concurrent parsing,
pause/cancel handling, progress callbacks, and preparation of chart records. It
writes through a repository batch that preserves current transactions and
prepared-statement reuse.

`DifficultyTableImporter` owns URL/file loading, JSON parsing, normalization,
and progress reporting. It passes validated domain records to one
`ChartRepository` transaction.

`ResultPersistenceCoordinator` continues to implement replay-outbox-first
result persistence. It coordinates `ReplayRepository` and `ScoreRepository`
without moving the multi-database workflow into either repository.

`ProfileSessionCoordinator` continues to own the profile-switch workflow and
its rollback behavior. It binds the score and replay repositories under the
existing global database activity gate.

`MusicPlayerService` continues to own playback behavior, locking, and in-memory
caches. It delegates persistence to `MusicPlaylistRepository` and no longer
stores a raw database handle.

## Data Flows

### Startup

1. `ApplicationContext` constructs repositories with the relevant global or
   active-profile database paths.
2. The startup initializer asks each repository to ensure readiness.
3. Every repository is attempted so the existing aggregate readiness status
   remains complete.
4. Runtime startup continues only when all required repositories are ready.

### Chart browsing

1. Main Menu retains a chart session.
2. The existing page cache asks the session for count, page, and index queries.
3. Score-backed filters acquire a scoped score-query lease so SQLite performs
   the same attached-database joins.
4. Cache revisions trigger the same reload and rebind behavior as today.

### Chart scanning and difficulty import

1. A service performs I/O and parsing outside the repository.
2. It opens an independent chart session and begins a domain write batch.
3. The batch reuses prepared statements and preserves the existing checkpoint
   transaction boundaries.
4. Successful material changes bump the library revision exactly when the
   current implementation does.
5. Cancellation commits the same current partial batch as today; checkpoint
   retention/clearing and storage-failure rollback follow the characterized
   current semantics.

### Result persistence and recovery

1. `ResultPersistenceCoordinator` stages a replay and pending score payload in
   `ReplayRepository`.
2. It projects the score idempotently through `ScoreRepository`.
3. It acknowledges the pending write through `ReplayRepository`.
4. Recovery repeats the same sequence and preserves all existing status and
   backlog behavior.

### Profile switching

1. `ProfileSessionCoordinator` acquires the existing switch guard.
2. Candidate score and replay connections are validated and migrated.
3. Both repositories are rebound, result recovery runs, and profile caches are
   refreshed.
4. Failure restores the previously validated repository paths/connections and
   reports the existing sanitized result.

## Error Handling

Repository application boundaries remain non-throwing. Existing
domain-specific outcomes for staging, projection, acknowledgment, and recovery
remain intact. Simpler methods keep their current success/failure semantics
rather than being forced into one generic result type.

RAII owns every connection, statement, error message, attachment, and
transaction. Low-level failures are translated and logged by the owning
repository. Detailed SQLite diagnostics stay in logs, while current sanitized
messages remain at user-facing boundaries.

Malformed replay and score rows retain their current skip-versus-fail policy.
Bounded replay scans retain their current candidate allowance and terminal
error behavior. All four validated-open paths reject future schemas, unreadable
schema versions, and failed integrity checks without mutating the database
family; score, replay, and music retain their existing invalid-migration
guarantees. The chart path's new fail-closed preflight is the one approved
observable change in this design.

## Performance Contract

Performance preservation is a correctness requirement for this refactor.

- Repository calls do not open a database per method.
- Chart readiness performs one full validated preflight; each later chart
  session performs exactly one SQLite connection open and no repeated snapshot.
- Main-menu chart browsing retains one connection, paging, and the bounded page
  cache.
- Background chart work retains independent connections and concurrent parsing.
- Attached score/chart joins remain SQL joins; they are not replaced by N+1
  lookups or full in-memory joins.
- Scan and import paths retain transaction batching and prepared-statement
  reuse.
- Score and replay caches retain revision-based invalidation.
- Music refreshes retain one connection and existing service-level caches.
- Existing limits, sort orders, corruption budgets, and indexes remain intact.
- No new global mutex serializes chart reads with background chart writes.

Deterministic tests use SQLite tracing or authorizers to enforce statement-count
budgets and `EXPLAIN QUERY PLAN` checks to detect lost index use. Wall-clock
benchmarks run locally against representative fixtures as supporting evidence,
not as timing-sensitive CI assertions.

## Testing Strategy

Add characterization coverage before changing each production slice.

- Chart count, page, and path-index results for existing filters and sorts,
  including score-backed queries.
- Folder clear-rank aggregation and statement counts.
- Score saves, best-score selection, cache construction, course recovery,
  migration results, and schema snapshots.
- Replay staging, recovery, bounded summary reads, full hydration, migration
  results, and schema snapshots.
- Playlist mutation, cache refresh, library selection, queue restoration, and
  player-state persistence.
- Chart scan batch commit/rollback, cancellation, progress, and revision
  behavior.
- Difficulty import replacement/rollback and progress behavior.
- Startup initialization and profile-switch rollback across repository objects.

Each repository cutover begins with a failing compile/API or boundary test,
then makes the smallest production change needed to pass while the existing
behavioral characterization remains green.

Focused tests run after each slice. Final verification runs the complete CTest
suite and the repository-prescribed desktop build:

```sh
cmake --build cmake-build-debug --target main -j 6
```

## Migration Sequence

1. Add behavior, schema, query-count, and query-plan characterization tests.
2. Consolidate shared private SQLite utilities under `src/repositories/`
   without changing SQL behavior.
3. Convert score and replay helpers, update result/profile coordinators and
   consumers, and remove their helper/singleton APIs.
4. Convert music persistence and remove raw connection ownership from
   `MusicPlayerService`.
5. Convert chart persistence to repository sessions, add the approved
   fail-closed chart preflight, and migrate Main Menu, Settings, startup, and
   other consumers.
6. Extract chart scanning and difficulty importing into application services.
7. Remove obsolete helper APIs and raw repository-related SQLite handling from
   scenes and services.
8. Run focused tests, full CTest, query-plan and statement-count checks,
   representative local benchmarks, and the desktop build.

## Acceptance Criteria

- No `ChartDBHelper`, `ScoreDBHelper`, `ReplayDBHelper`, or `MusicPlaylistDB`
  class remains.
- No repository exposes `GetInstance()`.
- All repository sources and their private SQLite helpers are grouped under
  `src/repositories/`.
- Scenes and application services do not own or close repository SQLite
  connections.
- Existing databases open with identical stored data and compatible schemas.
- Existing migrations produce identical schema and data results.
- Observable query ordering, filtering, recovery, cancellation, progress, and
  error behavior remains unchanged except that corrupt or future-version chart
  database families now fail closed without mutation.
- Deterministic statement-count and query-plan budgets do not regress.
- Representative local benchmarks remain within baseline noise or improve.
- Focused tests, the complete CTest suite, and the desktop `main` build pass.
