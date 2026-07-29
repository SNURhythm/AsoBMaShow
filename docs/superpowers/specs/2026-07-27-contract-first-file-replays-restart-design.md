# Contract-First File Replay Restart Design

## Context

The first file-based replay implementation grew from a storage change into a
cross-cutting rewrite of replay encoding, gameplay input capture, result
persistence, IR uploads, result recall, course continuation, profile archives,
and file lifecycle handling. Review repeatedly found the same invariant applied
to one producer or consumer but omitted from another. The resulting pull
request is retained as reference material, but its implementation is not the
base for this restart.

This design restarts from `develop` commit
`5935b7847ff197dc66ea43e8fecb86ff426d3d4c`. It uses the earlier pull request
only as a source of review evidence, independent Beatoraja fixtures, and code
that can be selectively reintroduced after a contract test requires it.

## Goals

- Store every new eligible chart or course replay as a Beatoraja-compatible
  `.brd` file under the active profile's Beatoraja replay layout.
- Stop storing per-event, touch-sample, and lane-cover rows in SQLite for new
  attempts.
- Keep result history, provenance, and postponed IR uploads independent from
  replay availability and replay bytes.
- Let users share or delete individual replay files without deleting result
  history.
- Preserve old database entries only as summary records with missing replays.
- Establish shared invariants and a contract matrix before changing runtime
  behavior.
- Deliver the rewrite in reviewable slices rather than one repository-wide
  cutover.

## Non-Goals

- Do not convert legacy replay events into BRD files.
- Do not reconstruct legacy results, IR snapshots, gauge histories, judgement
  timing, or replay input from legacy event rows.
- Do not support Watch, Retry Same, G-Battle, ghost playback, video export,
  replay sharing, or detailed View Result for legacy summary records.
- Do not retain a runtime schema-v10 replay reader after the schema transition.
- Do not make imported Beatoraja files eligible score or IR evidence.
- Do not preserve malformed legacy detail that is absent from the replay
  header.
- Do not use the restart to refactor unrelated gameplay or UI behavior.

## Selected Legacy-Record Approach

Legacy history uses dedicated, read-only summary tables. It does not share the
strict modern result tables.

A unified nullable result table was rejected because every modern reader and
validator would need to account for incomplete historical fields. Retaining the
old `replays` tables was rejected because it would preserve the replay/result
coupling and the event-row database growth this change is meant to remove.

Dedicated summaries keep both domains honest:

- modern results are complete, validated, and independently recallable;
- legacy summaries contain only facts already stored in legacy headers;
- neither model can be mistaken for the other; and
- replay capabilities are unavailable for every legacy summary without
  inspecting old events.

## Domain Boundaries

The implementation separates these durable concepts:

| Concept | Durable owner | Authority |
| --- | --- | --- |
| Modern chart/course result | SQLite | Complete result facts, provenance, creation context, and result fingerprint |
| IR submission snapshot | SQLite | Provider-neutral postponed-upload facts captured at completion |
| Replay playback | BRD file | Chart setup, raw logical input, touch visualization, timed lane-cover changes, and course rest timing |
| Replay file reference | SQLite | Contained relative path, content hash, size, codec version, and owning modern result |
| Legacy result summary | SQLite | Best-effort header facts copied without consulting replay events |
| Imported remote record | SQLite | Existing provider-owned remote score model; never local replay evidence |

Replay playback types must not contain result facts, attempt fingerprints,
`ScoreProvenance`, IR state, database IDs, or delivery state. Modern result and
IR fingerprints must not contain replay paths, hashes, availability, or bytes.

## Authoritative Invariants

### Replay setup

One canonical `ReplaySetup` model owns every fact required to interpret raw
input: chart hashes and key mode, effective long-note mode, random branch and
lane options, double-play option, assist option, initial gauge configuration,
ruleset descriptor, playback rate, initial lane cover, and other supported
stock Beatoraja configuration.

All producers and consumers use the same canonical validation entrypoint. A
locally recorded setup that passes capture must be accepted by the encoder and
decoder. A decoded setup must be checked against the selected chart before
playback or materialization.

### Limits and time

One replay-limits module owns bounds for pre-roll time, completion time, input
count, touch count, lane-cover count, course stage count, course rest duration,
JSON and gzip expansion, strings, and file size. Producers clamp or reject at
the boundary where a value originates; codecs enforce the same limits for
untrusted files. A local producer may never create data the local codec rejects.

Signed song time explicitly supports the established gameplay pre-roll. Event
ordering is monotonic and never repaired by sorting after capture.

### Identity and provenance

Chart identity is computed from parsed content and compared with recorded
identity before applying saved setup. Stored display metadata never overwrites
a contradictory parsed identity.

Result provenance describes the completed score. Replay setup describes how
to interpret input. Shared facts have one canonical comparison function, used
before binding a replay file to a result and before exposing replay-dependent
actions. Neither domain copies facts opportunistically from the other.

### Result facts

Modern result facts are captured directly when gameplay completes. Result
recall reads those facts without opening or materializing a replay. IR snapshots
are captured from the same completed-attempt facts and never reconstructed from
replay input.

Materialized replay outcomes may be compared with a saved modern result as an
integrity check, but they never become the source of that result.

### File ownership and availability

Every replay file has at most one modern result owner. Reservation, temporary
write, finalization, database association, retry, replacement, deletion, orphan
cleanup, profile duplication, and archive import use one explicit file-state
machine.

File absence is a normal state because the user may delete a replay. A missing
file disables replay-dependent capabilities without invalidating its modern
result, IR snapshot, outbox state, or receipts. Corrupt or mismatched files are
reported as unavailable and remain deletable.

### Course continuation

Course state has an explicit carried-state model covering stage index, total
stage count, score and maximum score, combo and maximum combo, each gauge's
state, adopted gauge, rest duration, constraints, and per-stage replay setup.
Live continuation, raw replay materialization, Watch, and video export consume
the same transition contract rather than maintaining parallel copies.

## Contract Matrix

Each supported cell records its authority, expected capabilities, failure
behavior, and test fixture. The initial matrix contains these dimensions:

| Dimension | Values |
| --- | --- |
| Record origin | New live chart, new live course, legacy chart summary, legacy course summary, imported stock BRD, imported remote result |
| Replay state | Verified present, user-deleted, missing, corrupt, mismatched, unsupported extension |
| Consumer | Records list, View Result, Watch, Retry Same, G-Battle, practice ghost, video export, share/copy, delete, IR upload, profile duplicate, profile archive |
| Setup axis | 5K, 7K, 9K, 10K, 14K, supported scratch directions, LN/CN/HCN, undefined LN, random/lane options, DP FLIP, supported gauges and rulesets |
| Course axis | Complete, partial, repeated chart, mixed stage setup, carried combo/gauge, bounded rest |

The matrix is not implemented as an exhaustive Cartesian product. Every value
receives a direct contract test, and pairwise fixtures cover interacting axes.
Known high-risk combinations from the previous review receive explicit tests.

The capability baseline is:

| Record | Records | View Result | Replay actions | New postponed IR |
| --- | --- | --- | --- | --- |
| Modern result + verified BRD | Yes | Yes | Yes | From saved snapshot when eligible |
| Modern result + absent/invalid BRD | Yes | Yes | No, except delete for an existing invalid file | From saved snapshot when eligible |
| Legacy summary | Yes | No | No | No reconstruction; preserve only independently durable existing work |
| Imported stock BRD without local result | Replay import surface only | No | Playback-only where explicitly supported | No |
| Imported remote result | Yes | Existing remote-detail capability | No | No |

## Legacy Schema Transition

The schema transition is a SQLite-only operation. It performs no replay file
I/O and never reads legacy event, touch, or lane-cover rows.

Within one transaction it will:

1. Validate the presence and shape of the expected legacy header tables.
2. Create strict modern result/snapshot/reference tables and dedicated legacy
   chart/course summary tables.
3. Copy legacy chart headers: stable legacy ID, chart path and hashes, title,
   artist, LN mode, final score, header max combo, final gauge, clear type,
   creation time, and independently stored provenance when valid.
4. Copy legacy course headers: stable legacy ID and key, names, constraints,
   aggregate score/max combo/final gauge/clear, completed and total stage
   counts, creation time, and independently stored provenance when valid.
5. Represent missing or malformed optional header values as unknown and mark
   the summary partial. No event-derived repair is attempted, and one damaged
   legacy record does not block otherwise structurally valid migration.
6. Preserve independently durable outbox payloads and receipts without
   manufacturing an IR snapshot. Work that requires replay reconstruction is
   retained only as non-runnable historical state with a bounded diagnostic.
7. Drop legacy replay-event, touch, lane-cover, replay, course-stage, and
   course-replay tables and their indexes.
8. Verify row counts, references, schema shape, and foreign keys, advance the
   schema version, and commit.

Structural SQL, allocation, or verification failure rolls the transaction back
and leaves the legacy database unchanged. Once the transaction commits, legacy
playback rows are intentionally and permanently discarded. Profile validation
must not require any legacy replay file after the transition.

## New Attempt Persistence

A completed modern attempt consists of three independently validated payloads:
the compact result, an optional IR snapshot according to eligibility policy,
and replay playback data according to replay capture policy.

The coordinator follows one recoverable state machine:

1. Validate the result, snapshot, replay setup, and shared-fact agreement.
2. Reserve one deterministic Beatoraja-compatible path for the attempt.
3. Encode to a private temporary file, flush it, atomically install it, and
   validate the installed bytes.
4. In one SQLite transaction, insert the modern result, snapshot/outbox work,
   and replay-file reference.
5. Mark the reservation finalized and reconcile ambiguous acknowledgements by
   exact attempt, result fingerprint, path, and file hash.

The implementation must define recovery for a file installed before a database
rollback, a database row whose file is later user-deleted, and an interrupted
retry. Cleanup may remove only files whose ownership is proven by the shared
state machine.

## Beatoraja Interoperability

New files use Beatoraja's gzip-compressed JSON envelope and compact `keyinput`
stream, pinned to an identified upstream revision and independent golden
fixtures. Chart and course filenames follow Beatoraja's replay directory,
hash stem, undefined-LN prefix, course constraint, and numeric slot/index
layout.

Initial lane cover uses the supported stock play configuration. Timed
lane-cover events, touch visualization, and other AsoBMaShow-only playback data
use a versioned extension ignored by stock Beatoraja. The extension contains
no result, provenance, IR, or database data.

## Delivery Slices

Work proceeds as a stack of small branches rooted in `develop`. Each slice
must be buildable and independently reviewable before the next begins.

1. **Contracts and characterization:** capture current `develop` behavior,
   add the contract matrix and pure invariant models, and add no runtime
   cutover.
2. **Codec and contained file store:** add pinned BRD fixtures, paths, codec,
   limits, and the isolated file ownership state machine without activating
   gameplay persistence.
3. **Modern result and IR independence:** add strict compact result and IR
   snapshot models/readers while retaining temporary adapters at existing
   call sites.
4. **Chart recording and playback:** capture raw logical input, persist new
   chart BRDs, and activate chart Watch/Retry/G-Battle/ghost/video consumers
   through shared setup and context contracts.
5. **Course recording and playback:** activate course BRDs through the shared
   carried-state contract.
6. **Profile and file actions:** activate availability, deletion, sharing,
   duplication, archives, and startup reconciliation through the shared file
   state machine.
7. **Summary-only schema transition and cutover:** migrate legacy headers,
   discard legacy playback rows, stop new event-row writes, and delete all
   temporary legacy runtime adapters.

If a slice crosses more than two domain boundaries or cannot be described by
one contract-matrix row group, it is split before review.

## Verification and Review Gates

Before production code in a slice, its contract or characterization test must
fail for the expected reason. Every review fix must add or strengthen a test at
the shared invariant boundary; line-local fixes without a contract test are not
accepted.

Verification includes:

- independent Beatoraja golden decode and encode checks;
- record-to-encode-to-decode closure tests proving local producers cannot emit
  locally invalid data;
- property and boundary tests for time, counts, sizes, enums, and arithmetic;
- result/replay/IR independence audits;
- chart identity and shared-fact authority tests;
- chart and course consumer matrix tests;
- SQLite migration tests proving no event table is read while copying legacy
  summaries and rollback preserves every legacy row;
- file-state fault injection at reservation, write, rename, validation,
  database stage, commit acknowledgement, cleanup, duplication, and archive
  boundaries;
- profile tests for verified, missing, user-deleted, corrupt, mismatched, and
  unreferenced files;
- focused tests for every slice, the full configured CTest suite, desktop
  `main` build, and iOS compile verification before final integration.

Automated review is requested once per completed slice, not after every fix.
The merge gate is no verified P1 or P2 violation of the documented supported
contract. Speculative behavior outside that contract is answered or deferred
rather than automatically expanding scope.

## Acceptance Criteria

- Fresh profiles create no per-event replay tables and new plays add no raw
  replay rows to SQLite.
- Every persisted modern replay is a validated BRD under the profile's
  Beatoraja-compatible path layout.
- Deleting a BRD disables replay-dependent actions only.
- Modern View Result and postponed IR succeed without opening a replay file.
- Legacy records remain visible as summary-only history and expose no replay
  or detailed-result capabilities.
- The legacy schema transition never reads events to derive facts and commits
  summary preservation plus playback-row deletion atomically.
- No runtime legacy replay reconstruction or event-row persistence remains
  after the final cutover.
- Producer and consumer paths share setup, limit, identity, result-comparison,
  course-state, and file-ownership invariants.
- Each delivery slice stays independently reviewable and passes its contract
  tests before the next slice begins.
