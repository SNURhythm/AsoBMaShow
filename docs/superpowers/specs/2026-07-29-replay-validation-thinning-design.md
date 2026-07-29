# Replay Validation Thinning Design

## Context

The file-replay branch added checks at capture, agreement, codec, context, and
persistence boundaries. Several of those layers reject the same replay for the
same fact. Worse, observations made while a live play is still finishing have
been used as hard validity bounds. Those checks can discard a complete local
capture before the BRD encoder is reached.

This addendum narrows validation without weakening the contract-first replay
design. Modern result history remains independent of replay files. Untrusted
files still fail closed, and a valid file still must belong to the selected
result and chart or course.

## Options considered

### 1. Boundary-owned validation (selected)

Each fact has one authority:

- The local capture boundary normalizes observer ordering and redundant input.
- The codec validates bounded BRD structure while encoding or decoding.
- The result-binding boundary checks chart/course identity, LN mode, and the
  setup facts needed to replay the saved attempt.
- The file store checks contained paths, ownership metadata, size, and hash.
- Persistence checks attempt ownership and performs atomic association.

Consumers do not repeat codec structural checks, and agreement functions do
not repeat playback validation. Optional parsed duration estimates do not
override completion bounds embedded in a structurally valid BRD.

### 2. Remove every replay check

This would make capture robust but could execute corrupt files or files for a
different chart. It violates the approved safety and identity contract.

### 3. Keep every check and improve diagnostics

This preserves the current failure modes. A better error message does not
restore a BRD that was discarded before persistence.

## Classification

### Normalize or diagnose; never discard a local replay

- Arrival order between realtime input producers.
- Touch and lane-cover observer ordering.
- Redundant press/release states and unmatched releases.
- A final accepted event observed just after the audio cursor snapshot.
- Exact agreement with an optional parsed-chart duration estimate.
- Re-simulated score, judgement, gauge, final-gauge, or provenance outcomes.

Local streams are stable-sorted. Redundant input states are removed. The
completion bound expands to the latest accepted event, provided a real
completion observation exists. Result re-simulation remains diagnostic.

### Reject at exactly one owning boundary

- Invalid limits, resource overflow, unsupported controls, non-finite touch
  coordinates, invalid lane-cover values, or timestamps before supported
  pre-roll.
- Malformed gzip/JSON/base64, unsupported codec extensions, or structurally
  unplayable BRD setup and event streams.
- Unsafe or non-contained paths, symlinks, size/hash disagreement, or an
  unsupported codec version.
- Wrong chart/course identity, key mode, applicable LN mode, or replay setup
  facts that select materially different playback behavior.
- Wrong attempt/result ownership, invalid result rows, course shape/rest/path
  constraints, or migration/association transaction failure.

These are safety, compatibility, identity, or atomicity rules. Removing them
would let the application execute unbounded data, play the wrong chart, or
attach a file to the wrong history record.

## Producer flow

At completion, all locally observed input, touch, and lane-cover streams are
normalized together. The bound is derived from the completion observation and
the latest accepted event. A missing completion observation, resource
overflow, or structurally unsafe local data drops only the replay attachment;
the modern result and postponed IR snapshot still persist.

The capture layer checks result binding after normalization. The BRD codec is
then the one structural authority for the file it writes.

## Consumer flow

The file store verifies ownership metadata and bytes. The codec validates and
decodes the BRD once. Context loading checks the decoded chart/course against
the selected result and parsed chart identity. It does not validate the same
playback envelope again and does not reject a valid embedded completion bound
merely because an optional parser estimate differs.

## Tests

- Realtime input arriving out of timestamp order remains attached.
- A final accepted event extends an observed completion bound.
- Interleaved touch and lane-cover observations are stable-sorted and encoded.
- Different optional parsed duration estimates do not disable a valid BRD.
- Invalid structure, wrong chart identity, wrong LN mode, unsafe files, and
  wrong attempt ownership remain rejected by their single authorities.
- Focused capture, codec, context, persistence, migration, and file-state tests
  plus the full configured suite remain green.

