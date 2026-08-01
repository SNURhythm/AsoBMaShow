# Thin Replay Runtime Contract Design

## Purpose

The file-replay branch currently treats a locally produced BRD as proof that
the current engine can reproduce every saved result fact. That makes replay
availability depend on observer-perfect input, duplicate-free transitions,
two equivalent BRD projections, and bit-exact re-simulation. The file migration
only requires a safe, owned, structurally playable BRD tied to the selected
chart.

This change separates safety and ownership checks, which remain runtime gates,
from reproducibility checks, which become diagnostics and regression-test
oracles.

## Runtime gates that remain

- Bounded gzip, JSON, Base64URL, string, event, and course-stage processing.
- Canonical relative replay paths and safe real filesystem entries.
- File size and SHA-256 agreement with the stored reference.
- Supported codec structure and structurally valid controls, timestamps, and
  setup values needed to construct playback.
- Attempt/result ownership, chart SHA-256/key-mode identity, and applicable
  long-note mode.
- Atomic database migration and file-association ownership.

## Checks that no longer block replay actions

- Exact equality between the stock Beatoraja projection and the Aso extension.
  Locally encoded files continue to emit both projections and compatibility is
  covered by focused encoding tests. The Aso extension is authoritative when
  present and supported.
- Re-simulated score, judgement timing, gauge history, final-gauge bits, and
  provenance equality. Re-simulation still constructs the compatibility event
  stream, but a result mismatch is retained as a diagnostic instead of making
  Watch, G-Battle, Retry Same, ghost, or video export unavailable.

## Producer normalization

Raw observer input is normalized before it becomes durable playback input.
Events are stable-ordered by song time. Duplicate state transitions and
unmatched releases are ignored. Unsafe controls, out-of-range timestamps, and
resource-limit overflow still reject capture because they cannot be encoded
safely.

The same normalizer is applied to both the ordinary input recorder and the
realtime gameplay worker's completed capture, preventing different producer
authorities.

## Replay attachment recovery

An exact retry of a result previously saved without a replay may add one replay
attachment. The result, IR snapshot, and outbox drafts remain immutable. The
attachment must pass the existing path, metadata, reservation, and ownership
checks. An existing attachment can never be replaced by a different one.

## Diagnostics

Consumer failures retain their precise diagnostic. Watch logs it instead of
silently resetting, and video export returns it instead of collapsing every
failure to `No Replay`. A playable result mismatch is logged as a warning but
does not stop the action.

## Testing

- Recorder tests prove decreasing timestamps are stable-ordered and redundant
  edges are normalized without invalidating the attachment.
- Realtime and chart-capture tests prove the shared normalizer is used.
- Codec tests prove supported Aso extensions remain authoritative when the
  stock projection differs, while generated stock fields remain compatible.
- Materializer/consumer tests prove a result mismatch still yields playable
  replay data and a diagnostic.
- Persistence/repository tests prove an exact summary-only retry can attach a
  replay once and cannot replace an existing attachment.
- UI contract tests prove precise consumer diagnostics reach Watch/export.

## Non-goals

- Legacy summary records remain summary-only and are not reconstructed.
- File hash, path safety, structural decoding, and chart identity checks are
  not weakened.
- No deployment or schema-version change is required.
