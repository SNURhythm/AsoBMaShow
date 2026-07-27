# Modern Course BRD Recording and Playback Design

## Scope

This is the focused design for delivery Slice 5 of the approved
contract-first file replay restart. It activates modern course result capture,
course BRD persistence, replay-independent course result recall, and the
course Watch, Retry Same, and video consumers. G-Battle and practice ghosts
remain chart-only. File deletion, sharing, profile duplication, archive
handling, and startup-wide reconciliation remain Slice 6 work. Legacy course
rows remain readable through their existing adapters until the summary-only
transition in Slice 7.

## Considered Approaches

The selected approach adds strict course tables in schema version 12, a
standalone course-continuation contract, and course capture/context/consumer
types parallel to the modern chart types. Chart and course persistence share
one file installation and association boundary, while their result validation
and score projection remain type-specific.

Two alternatives were rejected. Expanding every chart result, persistence,
and consumer type into a tagged chart/course union would spread conditional
course shape checks through otherwise strict chart code and make partial
course handling easy to omit. Reusing legacy `CourseReplayData` and adapting
it at storage or consumer edges would keep judged events and final result
facts as an accidental replay authority, preserve duplicate continuation
logic, and make Slice 7's legacy cutover unsafe.

## Durable Course Attempt

A completed or failed live course produces two independent payloads:

1. a `ModernCourseResult`, captured directly from each completed stage and
   the final course state; and
2. an optional `ReplayCourseDocument`, containing only each completed stage's
   canonical setup, raw logical input, touch/lane-cover extensions, time
   bounds, and bounded rest after that stage.

The modern result includes the complete course entry list as compact note and
play-length facts, while stage result rows contain only the contiguous
completed prefix. Repeated chart identities are valid and retain their stage
indices. A partial course therefore remains a complete result-history record
whose BRD, when present, replays only the completed prefix. No aggregate
score, judgement, gauge history, provenance, database ID, or IR fact is
encoded in the BRD.

Course score projection remains a result-domain operation. It is performed
from the captured modern course result and cannot read the BRD. The existing
IR provider accepts chart submissions only, so a course attempt does not
manufacture an IR snapshot; the course result and replay availability models
nevertheless remain independent in the same way as chart results.

## Explicit Continuation Contract

`CourseContinuation` is the sole transition authority shared by live play,
raw BRD materialization, Watch, and video export. Its state explicitly carries:

- current stage index and total stage count;
- cumulative score and maximum score;
- current combo and course maximum combo;
- all gauge values and survival-failure flags;
- the selected and adopted gauge configuration;
- the bounded rest duration following the completed stage;
- normalized course constraints; and
- the canonical replay setup used by the completed stage.

A transition accepts exactly one contiguous stage completion, validates
non-negative and overflow-safe score/combo values, validates the full gauge
snapshot, clamps no invalid input silently, applies `ReplayLimits` to rest,
and advances the index once. It rejects missing, repeated, or out-of-order
stages. The next gameplay stage restores gauge and combo solely from this
state. Result aggregation reads the same accumulated state plus the saved
stage result facts; it does not independently recompute a different carried
state.

The BRD never serializes this derived state. During playback, the shared raw
driver judges a stage, creates the same stage-completion input used by live
play, and advances the continuation contract. Saved modern result facts are
then compared with the materialized facts as an integrity check. They are
never replaced by materialized values.

## Schema and Ownership

Schema version 12 adds strict modern course result, entry, and completed-stage
tables. The existing modern replay-file table is rebuilt transactionally so a
file has exactly one modern chart or modern course result owner. The shared
reservation and stem-sequence tables remain generic by attempt ID and are not
duplicated for courses.

The course result, all entry facts, all completed-stage facts, and an optional
course replay reference are inserted in one SQLite transaction. Every durable
payload is canonically deserialized and revalidated on read. Exact retries
must agree on attempt ID, result fingerprint, every stage and entry row, path,
compressed hash, size, and codec version. A conflicting retry fails closed.

Chart and course persistence use one `ReplayFileAssociationCoordinator` for
path reservation, temporary write, atomic install, installed-byte validation,
occupied-slot retry, association acknowledgement, and exact-match cleanup.
Result-specific coordinators supply only the canonical stem, encoded bytes,
and transactional association callback. This keeps file ownership and cleanup
rules from diverging between chart and course paths.

## Course Path and Agreement

The course path stem is computed by the existing Beatoraja path contract from
the ordered chart SHA-256 list, effective long-note mode, undefined-LN facts,
and normalized course constraint identifiers. The numeric history slot is
allocated by the same reservation authority used by charts.

Before association, one course agreement function verifies:

- the total and completed stage shape;
- each completed stage's parsed content identity and key mode;
- each stage's effective long-note mode and canonical setup;
- result/setup shared provenance facts;
- ordered course identity, including repeated charts;
- normalized constraints and initial gauge policy; and
- bounded time and rest values.

The same agreement function runs when a course BRD is loaded. Stored display
metadata never replaces a contradictory parsed identity, and one stage
mismatch makes only replay-dependent actions unavailable.

## Context and Consumers

`CourseReplayContext` loads a strict modern course result, parses the ordered
completed chart prefix, derives identities and time bounds, verifies the file
reference and contained bytes, decodes a course BRD, validates each stage,
and applies the course agreement contract. It preserves the modern result in
its outcome whenever replay loading fails.

`CourseReplayConsumer` is the only modern course preparation pipeline. It
reproduces every stage setup, materializes the raw replay through the shared
driver and continuation contract, compares all materialized stage and
aggregate facts with the saved result, and only then returns the temporary
in-memory compatibility objects needed by current scenes and the video
renderer. Those compatibility objects are never persisted and are removed at
the Slice 7 cutover.

Records lists modern course results by durable course key. View Result uses
`ModernResultRecallBuilder` and does not load the BRD. Watch, Retry Same, and
video use `CourseReplayConsumer`; G-Battle and practice ghost are never
offered for a course. Missing, corrupt, unsafe, unsupported, or mismatched
files disable those replay-dependent actions without hiding the record or its
detailed result.

## Failure and Recovery

- Missing or invalid raw capture saves the modern course result without a
  replay reference.
- Encoding, reservation, installation, or verification failure saves the
  result without a replay reference when ownership is unambiguous.
- Database rollback after installation removes only an exact hash-and-size
  match proven to belong to the current reservation; ambiguous bytes remain
  for reconciliation.
- An interrupted exact retry reuses the deterministic reservation and
  identical installed bytes. A changed result, stage list, or file is an
  integrity conflict.
- A referenced BRD later becoming missing or invalid never changes result
  recall, course-score history, or any independently durable work.
- Rest above the one-hour limit, non-contiguous stages, arithmetic overflow,
  or invalid carried gauge state rejects the replay attachment rather than
  weakening the continuation contract.

## Slice Gate

Slice 5 is complete when new complete and partial course attempts save strict
modern results and Beatoraja-layout course BRDs without new legacy raw rows;
repeated-chart and mixed-setup courses round-trip; live play, materialization,
Watch, Retry Same, and video use the same continuation transition; modern
course View Result works with the BRD removed; and the complete diff review
against `origin/develop` finds no duplicate course setup, limits, identity,
result-agreement, continuation, or file-ownership authority.
