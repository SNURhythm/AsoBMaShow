# File-Based Beatoraja Replay Persistence Design

**Date:** 2026-07-25
**Branch:** `feature/file-based-replays`
**Reviewed base:** `5935b78`

## Context

AsoBMaShow currently stores every replay event, touch sample, and lane-cover
change as a separate SQLite row. A normal play therefore expands
`replays.db` by many rows plus indexes, and the database grows in proportion
to the total event count rather than the much smaller number of played
attempts.

The persistence model also conflates unrelated responsibilities.
`ReplayData` contains playback events and setup, chart and result metadata,
final score facts, database identity, and `ScoreProvenance`. A
`ChartResultAttempt` embeds that object and hashes the replay stream together
with the score and provenance. Historical IR submission then reconstructs a
result from replay events to recover facts that should have been durable
result data. Deleting or losing a replay consequently risks disabling result
recall and postponed IR upload even though neither operation should depend on
raw playback events.

Replay files are also a useful interoperability boundary. Beatoraja stores a
replay as gzip-compressed JSON in a `.brd` file. Its `keyinput` field is a
URL-safe Base64 encoding of a gzip stream containing fixed nine-byte input
records. Its reader ignores unknown JSON fields. Matching that envelope lets
AsoBMaShow replay files be shared without maintaining a second export-only
format.

The compatibility baseline is Beatoraja master commit
`5f46fe198e88abbefe9215ca2de397aef8f54bd8`, specifically its
[`ReplayData`](https://github.com/exch-bms2/beatoraja/blob/5f46fe198e88abbefe9215ca2de397aef8f54bd8/src/bms/player/beatoraja/ReplayData.java),
[`PlayDataAccessor`](https://github.com/exch-bms2/beatoraja/blob/5f46fe198e88abbefe9215ca2de397aef8f54bd8/src/bms/player/beatoraja/PlayDataAccessor.java),
and
[`PlayConfig`](https://github.com/exch-bms2/beatoraja/blob/5f46fe198e88abbefe9215ca2de397aef8f54bd8/src/bms/player/beatoraja/PlayConfig.java)
models. Golden fixtures pin behavior so a later upstream change becomes an
explicit compatibility update rather than silently changing stored data.

## Decisions

- Each recorded play owns one immutable `.brd` file under the active profile.
- The file uses Beatoraja's gzip-JSON envelope and stock replay fields.
- The profile directory and hash/LN-mode/index filename grammar match
  Beatoraja's replay path layout.
- SQLite stores result history, provenance, provider-neutral IR submission
  snapshots, delivery state, and a small replay-file reference. It never
  stores raw replay event rows for new records.
- Result integrity, IR proof, and retry do not include or read replay bytes.
- Result recall is built from the persisted result record, not reconstructed
  from replay events.
- A missing replay file leaves result history, IR snapshots, outbox work, and
  receipts intact.
- Existing row-based replays receive a one-way, crash-safe schema migration to
  `.brd` files. No legacy replay-reconstruction fallback remains at runtime.
- Legacy scratch direction cannot be recovered from the existing lane-shaped
  events. Migrated files remain faithful in AsoBMaShow through an extension
  playback track, while stock Beatoraja playback is best-effort for affected
  charts. New recordings capture raw logical input and do not have this
  limitation.

## Scope

This design covers persisted single-chart and course replays, result recall,
automatic and postponed IR uploads, replay deletion, profile duplication,
profile archive import/export, and migration of the current replay schema.

Practice, Auto Play, and replay playback retain their existing capture-policy
decisions. The design does not make those sessions eligible score attempts.
It also does not make imported Beatoraja files eligible for IR submission.

## Goals

- Bound SQLite growth to one compact result/reference record per play instead
  of one row per event.
- Produce directly shareable Beatoraja `.brd` data for every new recorded
  play.
- Match Beatoraja's `replay/` directory and chart/course filename stems while
  retaining more than its four native replay slots.
- Preserve AsoBMaShow-only touch visualization and timed lane-cover behavior
  without making stock Beatoraja reject the file.
- Make replay files independently deletable by the user.
- Make result display and postponed IR upload fully independent from replay
  availability.
- Preserve current development replay data through an all-or-nothing,
  resumable migration.
- Keep persistence idempotent across process termination and ambiguous write
  acknowledgements.

## Non-goals

- Do not reproduce timed lane-cover changes in stock Beatoraja; its replay
  model stores only the initial `PlayConfig` state.
- Do not claim score equality when a replay is rejudged by a different
  ruleset. File compatibility means the replay loads and its logical input is
  consumable.
- Do not make stock Beatoraja enumerate more than its four native replay
  slots. AsoBMaShow history indexes above three use the same filename grammar
  but must be copied to slot 0, 1, 2, or 3 for Beatoraja's UI to select them.
- Do not synthesize new IR eligibility for migrated development records that
  lack a provider-neutral IR snapshot. Existing outbox entries and receipts
  remain valid and are preserved.
- Do not retain the legacy result-from-replay reconstruction path after the
  migration.
- Do not rename the on-disk `replays.db` profile component in this change.
  Its internal models and tables are decoupled even though the established
  profile filename remains stable.

## Considered Approaches

### 1. Beatoraja-compatible files plus compact SQLite references — selected

The filesystem owns immutable playback streams while SQLite owns queryable
result, provenance, IR, and file-reference metadata. This minimizes database
growth, gives users ordinary files they can share or delete, and gives each
integrity domain a narrow purpose.

### 2. One compressed replay blob per SQLite row

A compressed blob would eliminate event-row overhead and preserve an ordinary
SQLite transaction, but the database would still absorb all replay bytes.
Users could not delete or share one replay as a file without an export path,
and the replay/provenance model boundary would remain easy to blur.

### 3. A private compact binary replay format

A private format could be smaller and simpler to validate. It would require a
separate Beatoraja exporter and make every shared replay a conversion result.
That conflicts with interoperability being a primary storage property.

### 4. Stock `.brd` fields only

An exact stock-only file cannot represent timed lane-cover changes, touch
samples, course rest timing, or the authoritative legacy event stream needed
to preserve existing AsoBMaShow replays. A namespaced extension is required,
but it contains only playback data and never result provenance.

## Domain Boundaries

The implementation separates five concepts:

| Concept | Durable owner | Contents |
| --- | --- | --- |
| `ReplayPlaybackData` | `.brd` file | Chart playback setup, raw logical input, touch visualization, timed lane-cover changes, and course playback timing |
| `PersistedChartResult` | SQLite | Attempt identity, chart identity/display data, score and judgement totals, gauge/timing summaries, clear state, play time, and `ScoreProvenance` |
| `IrSubmissionSnapshot` | SQLite | Versioned provider-neutral submission facts and ruleset proof inputs captured at completion |
| `ReplayFileReference` | SQLite | Beatoraja filename stem/history index, relative path, content SHA-256, codec/version, compressed size, and record association |
| `CompletedChartAttempt` | Memory only | A short-lived command that coordinates the independently valid result, IR snapshot, replay payload, and automatic provider drafts |

`ReplayPlaybackData` cannot include `ScoreProvenance`, IR eligibility, provider
payloads, final score facts, database IDs, or delivery state.
`PersistedChartResult` and `IrSubmissionSnapshot` cannot include replay events
or a replay hash in their integrity fingerprints. The attempt ID associates
the records, but no domain derives its truth from another domain's bytes.

A minimal playback ruleset ID/revision and starting playback configuration may
appear in the AsoBMaShow extension because they change how AsoBMaShow
interprets input. They are playback configuration only: the IR pipeline never
reads them, never treats them as proof, and never compares them with stored
eligibility.

## Profile File Layout

Add `replayDirectory` to `PlayerProfilePaths`, resolved as:

```text
<profile>/replay/
```

This is the direct analogue of Beatoraja's
`<playerpath>/<player>/replay/` directory.

Single-chart filenames use Beatoraja's exact stem and index grammar:

```text
replay/<ln-prefix><lowercase-sha256><slot-suffix>.brd
```

`ln-prefix` is empty for charts without undefined long notes. For charts whose
LN interpretation is mode-dependent it is empty for LN mode 0, `C` for mode
1, and `H` for mode 2. `slot-suffix` is empty for history index 0 and
`_<history-index>` for every positive index. Examples are:

```text
replay/0123...cdef.brd
replay/C0123...cdef_1.brd
replay/H0123...cdef_27.brd
```

Course files use Beatoraja's course stem and contain its JSON array of
per-stage replay objects. The stem concatenates the first ten SHA-256
characters of every stage in order, applies the same LN prefix, appends the
two-digit Beatoraja IDs for applicable constraints, then applies the same slot
suffix:

```text
replay/<ln-prefix><stage-1-sha-prefix>...<stage-n-sha-prefix>[_<constraints>][_<history-index>].brd
```

The constraint adapter reproduces Beatoraja's enum ordering and excludes its
`CLASS`, `MIRROR`, and `RANDOM` markers as Beatoraja does. AsoBMaShow-only
constraints remain in the result/extension data and do not create a private
filename grammar.

Beatoraja's
[`MusicSelector`](https://github.com/exch-bms2/beatoraja/blob/5f46fe198e88abbefe9215ca2de397aef8f54bd8/src/bms/player/beatoraja/select/MusicSelector.java)
exposes four indexes: 0 through 3. AsoBMaShow treats that index as an
unbounded, monotonically allocated history index for each complete filename
stem. It never overwrites or reuses an index, even after a file is deleted.
Files at indexes 0 through 3 are directly discoverable by stock Beatoraja.
Files at higher indexes still follow its path formula but are outside its UI's
four-slot scan. Sharing such a replay copies the unchanged bytes to a chosen
0..3 destination slot; only the filename changes.

Migration assigns indexes in ascending creation-time/public-ID order within
each stem, producing deterministic paths. SQLite stores only the normalized
relative path and allocated history index beneath `replay/`; absolute paths,
`..`, links, and path aliases are rejected.

## `.brd` Codec

### Outer envelope and stock fields

The complete file is UTF-8 JSON compressed with gzip. A chart replay is one
JSON object and a course replay is an array of those objects, matching
Beatoraja's reader.

AsoBMaShow writes the applicable stock fields from Beatoraja's `ReplayData`,
including:

- `player`, `sha256`, `mode`, `gauge`, `date`, and `config`;
- `rand`, chart option fields and seeds, double-play option, and lane shuffle
  information;
- `keyinput` as the compact logical input stream.

The codec owns explicit enum and key-mode mapping tables. Unknown AsoBMaShow
enum values fail encoding instead of silently producing a different stock
option. Compatibility fixtures cover supported 5-key, 7-key, 9-key, 10-key,
14-key, and other enabled application modes.

### `keyinput` encoding

New gameplay records logical transitions before judgement or replay-event
derivation. Each transition contains a monotonic song-relative microsecond
timestamp, a mode-specific logical control including scratch direction, and a
pressed/released state.

The stock stream is encoded exactly as Beatoraja expects:

1. Map the logical control to a Beatoraja key code.
2. Emit one signed byte: `keyCode + 1` for press and
   `-(keyCode + 1)` for release.
3. Emit the timestamp as a little-endian signed 64-bit integer.
4. Gzip the concatenated nine-byte records.
5. Encode that inner gzip stream with URL-safe Base64 as `keyinput`.

Miss, mine, gauge, combo, and judgement events are not synthesized into the
stock key log. Both clients derive those outcomes from the chart, inputs, and
their active ruleset.

### Initial and timed lane cover

The starting AsoBMaShow lane-cover percentage maps to Beatoraja
`config.lanecover` and its enabled state. Other representable starting visual
settings map to their `PlayConfig` equivalents.

Beatoraja does not record live lane-cover adjustments as key input. Later
changes therefore live in `asobmashow.laneCoverEvents`, preserving
`songTimeMicros`, `noteStartPositionPercent`, and
`resetVisibleTimeReference`. Stock Beatoraja ignores this unknown top-level
extension and uses the initial configuration only.

### AsoBMaShow extension

The optional top-level `asobmashow` object is versioned independently from the
stock envelope. It contains only playback-relevant additions:

- `schemaVersion`;
- minimal playback ruleset ID/revision when required;
- touch samples used for replay visualization;
- timed lane-cover events;
- per-stage rest timing for a course;
- a `legacyPlaybackEvents` track only on migrated files that cannot be
  represented faithfully by reconstructed stock input.

The extension never contains `ScoreProvenance`, eligibility, an IR proof or
payload, credentials, receipts, final result summaries, or SQLite identities.
The legacy track may retain existing per-event judgement, gauge, combo, and
score annotations when they are required for faithful AsoBMaShow playback,
but they are untrusted playback annotations rather than persisted result
facts. The track is accepted only for playback and video export. No result or
IR code may use it to reconstruct a score.

### Reading and validation

The reader accepts both stock Beatoraja files and AsoBMaShow-extended files.
It applies bounded gzip decompression, bounded JSON depth/string/array sizes,
bounded inner `keyinput` expansion, a maximum event count, monotonic timestamp
validation, key-code/mode validation, finite touch coordinates, and
lane-cover range checks. Trailing partial nine-byte input records are invalid
rather than ignored.

The file's stock `sha256` must identify the requested chart. An imported stock
file has no trusted local result or provenance and is replay-only. Unknown
stock fields remain forward-compatible; an unsupported future AsoBMaShow
extension version disables Aso-specific playback rather than misinterpreting
it.

## Result and IR Persistence

### Persisted result

The SQLite result record contains all facts needed by Records and the result
screen: chart identity and display metadata, key and LN modes, score/max
score, judgement and timing totals, combo values, adopted gauge history,
final gauge, clear type, play timestamp, and canonical `ScoreProvenance`.

Result recall constructs its read-only presentation state directly from this
record. It may parse the chart for presentation assets, but it does not replay
input, compare replay outcomes, or require the `.brd` file. Therefore `View
Result` remains available after replay deletion.

The result fingerprint is a versioned canonical hash of result and provenance
fields only. Replay path, replay SHA-256, replay availability, and replay bytes
are deliberately excluded.

### Provider-neutral IR snapshot

Every new eligible chart result captures a validated `IrSubmissionSnapshot`
at completion, before any provider-specific payload is built. It contains the
current `IrSubmission` facts: attempt/chart identity, key mode, result and
judgement totals, timing breakdown, adopted gauge history, play timestamp, and
provenance. It has its own schema version, canonical serialization, and
SHA-256 integrity value.

The snapshot is persisted in the same SQLite transaction as the result.
Automatic upload creates provider drafts from the in-memory snapshot.
Postponed/manual upload loads the stored snapshot and creates a provider draft
from it. Retry uses the already stored provider outbox payload. None of these
paths loads a replay file or calls result reconstruction.

Existing provider outbox rows and receipts remain authoritative. A replay
file may be deleted while an upload is queued, active, blocked, failed, or
awaiting a remote result without changing that state.

### SQLite shape

The new schema uses result terminology even though the profile database
filename remains `replays.db`:

- `chart_results` holds single-chart result and provenance data while
  preserving public integer IDs and canonical attempt IDs;
- `course_results` and lightweight `course_result_stages` hold course result,
  provenance, stage identity, and ordering without input events;
- `replay_files` holds the relative path, file SHA-256, codec/version, byte
  size, Beatoraja stem/history index, record kind, and record ID;
- `replay_file_reservations` temporarily owns an attempt's monotonically
  allocated history index and relative path until file/result finalization;
- `ir_submission_snapshots` holds provider-neutral, versioned snapshots;
- pending score projection, IR outbox, receipts, and remote mirror tables are
  retained and relinked to result/attempt identity rather than replay rows.

The row-per-event `replay_events`, `replay_touch_samples`, and
`replay_lane_cover_events` tables are removed at migration cutover. The old
course replay/stage tables are replaced after their result identity and stage
metadata have been copied.

## New-Result Commit Protocol

SQLite and the filesystem cannot participate in one literal transaction. The
coordinator therefore orders durable operations so SQLite never commits a
reference to a file that was not finalized:

1. Validate the independent result, IR snapshot, and replay payload.
2. In a short SQLite transaction, idempotently reserve the next history index
   for the Beatoraja filename stem. The reservation is keyed by attempt ID,
   unique by stem/index and path, and is not visible as a result or replay-file
   reference.
3. Encode the replay to a private temporary file in the target replay
   directory.
4. Flush the file, close it, atomically rename it to the reserved final
   path, and sync the parent directory where the platform supports that
   durability contract.
5. Decode the final file, validate its chart/mode and extension, and compute
   its SHA-256 and byte size.
6. In one SQLite transaction, insert or verify the result, replay reference,
   IR snapshot, pending score projection, and any automatic IR drafts, then
   consume the matching reservation.
7. Project and acknowledge the score through the existing idempotent
   cross-database coordinator.

The attempt is not reported durable until the file and result transaction are
both confirmed. A retry with the same attempt ID accepts an existing file only
when its bytes hash to the expected value and its decoded identity matches.
It accepts an existing result only when its independent result and snapshot
fingerprints match. A retry reuses its attempt reservation rather than
allocating a second filename.

A crash before file creation leaves a retryable reservation. A crash before
rename leaves the reservation and a temporary file. A crash after rename but
before result commit leaves the reservation and its finalized file, never a
database replay reference to an unfinished file. Retry resumes from that exact
reservation after validating any existing bytes. Startup removes only stale
temporary files and abandoned reservations that cannot belong to durable
attempt work. It never invents a result from an orphan replay.

## Atomic Legacy Migration

The migration is one-way. It preserves current development replay playback
and result history, but no legacy reconstruction API remains afterward.

Since a filesystem rename cannot be rolled back by SQLite, migration uses an
all-files-first staging phase followed by one database cutover transaction:

1. Acquire the profile/database write guard before repositories or scenes can
   observe the profile.
2. Read the complete legacy replay/course snapshot and validate all source
   rows needed for migration.
3. Group rows by their exact Beatoraja filename stem and assign monotonically
   increasing history indexes in creation-time/public-ID order.
4. For each replay, encode a deterministic sibling temporary `.brd`, flush
   and rename it to that stem/index path, decode it again, and record its
   SHA-256 and size in memory. Existing deterministic files are reused only
   after exact validation.
5. Translate legacy `Press`/`Release` events into the stock `keyinput` stream
   where possible. Preserve the complete old event, touch, and lane-cover
   playback in the AsoBMaShow extension when stock input is insufficient.
6. Revalidate every final file immediately before cutover.
7. Begin one SQLite transaction, create the new result/reference/snapshot
   schema, copy header facts and derive any missing result aggregates from the
   already loaded legacy events while preserving public IDs, insert every
   validated file reference, relink durable pending work, verify counts and
   foreign keys, drop the legacy replay-event structures, and bump
   `user_version`.
8. Commit once. Only this commit makes the new schema visible.

No legacy row is deleted or changed during file staging. If encoding, disk
space, validation, or the final database transaction fails, the old schema
remains authoritative and migration reports a blocking diagnostic. On the
next launch, deterministic validated files are reused and incomplete
temporaries are replaced. Thus interruption can leave harmless staged files
but cannot expose a partially migrated database.

Existing IR outbox payloads and receipts are copied unchanged. Migrated
records receive a provider-neutral snapshot only when one was already stored
independently; the migration does not reconstruct a new IR submission from
legacy replay events. Consequently an old record without a snapshot remains
viewable and replayable but cannot initiate a new postponed upload. This is
acceptable for unreleased development data and prevents a permanent legacy
coupling path.

Legacy events do not preserve raw scratch direction reliably. The extension
track remains authoritative for AsoBMaShow playback of those files. Their
stock `keyinput` is explicitly best-effort for BSS and other direction-sensitive
scratch behavior. New raw-input recordings are fully mapped.

## Replay Availability and Deletion

`ReplayFileReference` remains after a file is removed. Availability is derived
by resolving the safe relative path, requiring a regular non-link file, and
checking the stored hash before playback or export. It is not inferred from a
stale database boolean.

If the user deletes a `.brd` manually or through the app:

- Records and `View Result` remain available from `PersistedChartResult`;
- pending and completed IR state remains available;
- Watch Replay, replay video export, and Share Replay become unavailable;
- no database row, score, provenance, snapshot, outbox entry, or receipt is
  cascaded away.

An app delete action removes only the resolved file after confirmation and
reports that score history will remain. Restoring the exact bytes at the same
path restores availability because the stored SHA-256 still matches. A
different file at that path is treated as corrupt, not silently adopted.

## Profile Lifecycle and Portable Archives

Profile creation creates the replay directory as a private directory. Profile
duplication, legacy profile migration, and overwrite installation stage the
database and replay directory together before the existing durable profile
rename. Profile validation accepts missing user-deleted replay files but
rejects unsafe paths, links, and replay-directory escapes.

Portable profile archives include `replay/` recursively. Archive manifests
and `checksums.sha256` cover every included `.brd`; member-count, per-file,
compressed, expanded, and aggregate size limits prevent replay files from
bypassing current archive budgets. Import validates names and contents in a
private workspace before copying them into profile staging.

Export takes the replay database snapshot and replay-file copy under the
profile database activity guard so an app-driven replay write/delete cannot
produce a mixed archive. A concurrently missing externally deleted file is
treated as unavailable and omitted only when the database snapshot already
permits missing replay data; a changed/hash-mismatched file fails export
rather than exporting ambiguous bytes.

## Code Boundaries

- `BeatorajaReplayCodec` owns stock JSON mapping, nested `keyinput`
  encoding/decoding, gzip, extension parsing, and bounded validation.
- `ReplayFileStore` owns contained paths, temporary writes, durable rename,
  hashing, availability, deletion, and orphan/temporary cleanup.
- Gameplay input capture owns raw logical transition recording before
  judgement derivation.
- `ResultPersistenceModel` owns result records and result fingerprints without
  depending on replay types.
- `IrSubmissionSnapshot` capture/validation owns provider-neutral postponed
  upload facts without depending on replay types.
- The result repository owns SQLite result, provenance, replay-reference,
  snapshot, score projection, outbox, and receipt transactions.
- `ResultPersistenceCoordinator` coordinates these owners but does not merge
  their models or fingerprints.
- Result recall consumes result records. Replay playback and video export
  consume replay files. IR drivers consume submission snapshots.
- A migration-only decoder may read legacy replay rows. It is not linked into
  normal result recall or IR submission paths.

## Failure Semantics

- Invalid new replay payload: do not finalize a file or commit a result.
- Replay temporary-write, flush, rename, or read-back failure: keep the result
  unstaged and retryable in memory; do not project the score.
- Result/snapshot transaction failure after file finalization: retain an
  idempotently reusable orphan file and report the attempt unsaved.
- Missing user-deleted file: keep the result valid and mark only replay
  actions unavailable.
- Hash mismatch or unsafe path: fail replay actions closed and show a bounded
  corruption diagnostic; never use the bytes for playback.
- Migration failure before cutover: retain the old schema and all old rows.
- Migration transaction failure: roll back all database changes and retry
  from the old schema.
- Unsupported future `.brd` extension: preserve stock playback when safely
  possible, otherwise fail playback without mutating result or IR data.
- Invalid or missing IR snapshot: suppress new manual upload for that record;
  never fall back to replay reconstruction.

## Verification

Focused coverage will include:

- golden `.brd` fixtures produced by Beatoraja load in AsoBMaShow;
- AsoBMaShow-produced chart and course fixtures decode with a small
  Beatoraja-compatible reference reader;
- byte-level `keyinput` tests cover press/release sign, little-endian time,
  URL-safe Base64, inner gzip, logical lane mapping, and scratch direction;
- path fixtures cover Beatoraja's undefined-LN prefixes, unsuffixed slot 0,
  numeric slot suffixes, course hash concatenation, constraint suffixes, and
  the four-slot UI boundary;
- concurrent/retried reservations allocate one stable history index per
  attempt, while deletion never causes an old index to be reused;
- stock files ignore the AsoBMaShow extension while AsoBMaShow round-trips
  touch, timed lane cover, course rests, and migrated legacy playback;
- malformed gzip, decompression bombs, excessive arrays, invalid key codes,
  non-monotonic time, partial input records, traversal paths, symlinks, and
  hash mismatches fail closed;
- a new attempt leaves only one compact result row, one replay reference, one
  IR snapshot, and one file regardless of input count;
- result recall and postponed/manual upload succeed after the replay file is
  deleted;
- automatic and retry uploads never open a `.brd` file;
- fault injection at every file-write, sync, rename, read-back, database-copy,
  schema-cutover, and commit boundary proves migration is resumable and the
  old database remains intact before cutover;
- migration preserves record IDs, result/provenance facts, course order,
  pending score work, outbox rows, and receipts, then removes all event rows;
- legacy scratch fixtures use the AsoBMaShow extension and are identified as
  best-effort in stock compatibility tests;
- profile duplicate and archive round trips include replay files, tolerate
  deliberately missing files, and reject unsafe or changed files;
- deleting a replay never deletes a result, IR snapshot, outbox row, or
  receipt.

Repository verification includes the focused codec/file-store/migration,
result persistence, result recall, IR, profile, and archive suites; the full
configured CTest suite; `git diff --check`; and the desktop `main` build.

## Acceptance Criteria

- New recorded plays add no rows to event/touch/lane-cover tables because
  those tables no longer exist.
- Every successfully persisted play has a validated immutable `.brd` and a
  compact hash/path reference.
- AsoBMaShow stores `.brd` files under Beatoraja's `replay/` directory and
  hash/LN-mode/index filename grammar.
- A history file above Beatoraja's fourth native slot can be shared without
  content conversion by copying it to a selected 0..3 slot filename.
- Initial lane cover is visible in stock Beatoraja, and timed changes remain
  available when the same file is replayed in AsoBMaShow.
- A deleted replay file disables only replay-dependent actions.
- Every new postponed IR upload uses a persisted provider-neutral snapshot;
  there is no replay reconstruction fallback.
- Current row-based development replay data either migrates completely in one
  database cutover or remains entirely on the old schema with a diagnostic.
