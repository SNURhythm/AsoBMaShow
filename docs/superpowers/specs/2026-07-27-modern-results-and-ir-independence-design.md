# Modern Results and IR Independence Design

## Scope

This document narrows delivery Slice 3 of the approved contract-first file
replay design. It adds strict, replay-free chart and course result models,
provider-neutral postponed-IR snapshots, and result-recall readers that need a
chart but never need replay bytes. It does not change the SQLite schema or
activate file-backed gameplay persistence; those remain in later slices.

Existing `ReplayData` persistence and recall remain temporarily callable so
the branch stays buildable. They are adapters for the legacy schema, not the
definition of a modern result. In particular, their replay-inclusive payload
fingerprint must never be copied into a modern result fingerprint.

## Considered Shapes

Three shapes were considered:

1. Extend `ChartResultAttempt` in place. This keeps call sites small but leaves
   the modern model's public header dependent on `ReplayData` and makes replay
   mutation part of result integrity.
2. Store a result-shaped JSON blob inside the BRD extension. This violates the
   durable ownership boundary and makes result recall depend on a user-owned
   file.
3. Introduce replay-free result models and adapt current call sites until the
   schema cutover. This preserves the approved boundaries and supports a
   staged implementation.

The third shape is selected.

## Domain Model

`ModernResult.h` is the only public owner of compact local result facts. It
must not include or name `ReplayData`, replay playback collections, replay
paths, replay hashes, file state, IR delivery state, or SQLite.

A modern chart result contains:

- database identity, which is excluded from the content fingerprint;
- one required canonical attempt UUID and positive completion timestamp;
- chart storage/display identity, hashes, effective long-note mode, and key
  mode;
- score, maximum score, combo, judgement, fast/slow, final gauge, clear, and
  adopted-gauge facts;
- optional detailed judgement timing captured at completion;
- complete independently stored `ScoreProvenance`; and
- a canonical result fingerprint over every durable content fact.

A modern course result additionally contains course identity and labels,
constraints and requested setup, aggregate facts, the completed ordered stage
prefix, and small entry facts for every course slot. Stage maximum combo is the
carried course maximum and therefore may not decrease. Aggregate score,
maximum score, maximum combo, provenance, and stage order must agree with their
authoritative components.

The result ID and stored fingerprint field are excluded from fingerprint
input. Every other durable result field is covered. Floating-point values are
fingerprinted by their bit patterns, including signed zero.

## Validation and Shared Agreement

Capture and every untrusted reader use the same validators. Validation is
fail-closed and checks canonical identity, UUIDs, timestamps, enum ranges,
bounded strings and collections, arithmetic without overflow, score equations,
finite gauges, judgement timing totals, course prefix/order/aggregate rules,
provenance serialization, and fingerprint agreement.

One result-fact comparison entrypoint compares the facts shared by a saved
result and a materialized playback outcome. Database IDs, completion time, and
fingerprints are not shared playback facts. Later replay binding and consumer
activation must call this entrypoint rather than comparing local subsets.

Small cross-domain limits and lowercase digest syntax are implemented once in
root-level headers and consumed by both replay and result code. A result module
does not import replay storage policy merely to validate a hash or course
count.

## Postponed IR Snapshot

An IR snapshot is captured only from a validated modern chart result. It is a
versioned, provider-neutral payload containing the exact submission facts that
exist at completion. It contains no replay path, file hash, availability,
playback collection, result database ID, outbox state, provider receipt, or
delivery retry state.

Snapshot JSON is canonical and bounded. Serialization validates the in-memory
payload and its SHA-256 fingerprint. Deserialization requires the exact key
set, supported schema version, canonical byte spelling, valid provenance,
valid result arithmetic, and optional agreement with the fingerprint stored by
the database row. Unknown fields and unknown versions fail closed.

The existing `IrSubmission` builder gains a modern-result input. The old
replay-coupled attempt overload remains only as a compatibility adapter until
the final cutover.

## Replay-Free Result Recall

Modern chart recall validates the result, loads the selected chart by the
stored chart path, verifies parsed SHA-256/MD5 identity, then constructs a
read-only `RhythmState` solely from result facts. Stored display metadata may
fill the result presentation only after parsed identity agrees. No BRD or raw
event collection is opened or materialized.

Modern course recall applies the same rule to each completed stage, uses full
entry facts for the uncompleted suffix, and constructs result-browse state from
the saved aggregate and per-stage facts. It does not reconstruct course state
from playback. The carried-state replay contract remains Slice 5 work.

Missing chart content may still prevent rich result presentation; missing
replay content may not. The existing legacy recall overload remains unchanged
in this slice and is removed at the schema cutover.

## Failure Behavior

- A malformed result is rejected before chart loading.
- A missing chart returns an unavailable-result diagnostic.
- A chart identity mismatch does not allow stored headers to overwrite parsed
  identity.
- A missing, corrupt, mismatched, or user-deleted BRD has no input to result
  validation, recall, or IR snapshot reading.
- A malformed snapshot disables postponed upload only; it does not invalidate
  the result.
- Legacy replay-inclusive fingerprints remain legacy-only and are never
  accepted as modern result or snapshot fingerprints.

## Slice Exit Gate

The slice is complete when replay-independence, validation, fingerprint,
agreement, snapshot round-trip/tamper, and chart/course recall tests pass; the
source boundary test proves modern result and snapshot headers contain no
replay-file or raw-playback authority; the complete diff is reviewed against
`origin/develop`; the full configured CTest suite passes; and desktop `main`
builds.
