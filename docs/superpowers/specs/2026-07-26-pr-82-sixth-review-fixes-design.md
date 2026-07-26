# PR #82 Sixth Review Fixes Design

## Scope

Address the three verified defects submitted against commit `7c7b4f3b`:

1. Decode stock Beatoraja survival gauges with their real starting value.
2. Preserve both realtime lane transitions emitted by a directional scratch
   reversal.
3. Reclaim abandoned replay reservations when a profile database is opened.

Also correct the hand-built Beatoraja fixture so its displayed
`laneShufflePattern` agrees with its authoritative option and seed. The review
request to apply `laneShufflePattern` directly is intentionally not
implemented: pinned upstream Beatoraja reconstructs replay charts from
`randomoption` and `randomoptionseed`, then records the pattern as redundant
display metadata.

This work does not alter the BRD extension schema, replay path grammar,
database schema version, deferred slot-relocation work, or GitHub thread
state.

## Verified Root Causes

### Stock Starting Gauge

`decodeStockSetup()` maps the stock `gauge` field to `initialGaugeType` but
leaves `ChartPlaybackSetup::startingGaugePercent` at its 20% default.
`applyReplayPlaybackToStartOptions()` treats the field as an explicit value,
so HARD, EX-HARD, HAZARD, and course gauges start at 20% instead of the
configured profile's 100% initial value.

### Realtime Scratch Reversal

`LogicalGameplayInputAdapter` calls `releaseLane()` and `pressLane()` before
it emits the two corresponding applied callbacks during a reversal.
`RealtimePhysicalInputRouter` stores only one pending physical transition, so
the press overwrites the release. The first callback consumes the press and
deduplication suppresses it because the lane is still published as held; the
second callback finds nothing. The gameplay worker receives neither edge.

### Abandoned Replay Reservations

`reserveReplayFile()` advances `replay_stem_sequences` when it inserts a
reservation. Only successful result staging consumes the reservation. After
an interrupted or abandoned save with no final BRD, both the row and the
sequence high-water mark survive every restart, permanently consuming visible
Beatoraja history indexes.

### Inconsistent Fixture Pattern

The Java fixture generator manually supplies a lane permutation unrelated to
its RANDOM seed. Genuine Beatoraja replay setup applies the saved option and
seed and computes the displayed permutation from that modifier. Making the
fixture agree with the seed tests the same contract that Beatoraja uses.

## Considered Approaches

### 1. Targeted boundary fixes (selected)

- Reuse the gameplay gauge profile's canonical initial-value function when
  stock decoding has resolved the key-mode/profile combination.
- Replace the router's single pending transition with an ordered queue and
  pair one pending physical edge with each applied logical edge.
- On opening a new replay database session, delete only reservations that
  have no result and whose canonical final BRD is absent, then rebuild stem
  sequence high-water marks from durable replay files and retained
  reservations in the same database transaction.
- Generate the fixture pattern from its stock option and seed.

This preserves runtime idempotency and genuine Beatoraja playback semantics.

### 2. Make the stock permutation authoritative

Applying `laneShufflePattern` would play the committed inconsistent fixture,
but it would diverge from pinned Beatoraja, which uses the option and seed for
reconstruction. It would also require a second lane-remapping representation
in the raw replay model and conflict resolution when redundant fields differ.

### 3. Reuse reservation indexes immediately after every save failure

The persistence coordinator could delete a reservation as soon as one save
attempt fails. That breaks same-attempt retry idempotency and could overwrite
a final file installed just before a reported durability failure. Recovery
must be restricted to a new database session after the old attempt can no
longer resume.

## Design

### Gauge Initialization

After stock key mode and gauge profile are known, set
`startingGaugePercent` to `gaugeInitialValue(initialGaugeType, gaugeProfile)`.
Keep Aso-extension decoding unchanged because its explicit value is part of
the file contract. Normal gauges therefore retain 20% (30% for PMS where
applicable), while survival and course gauges start at 100%.

### Ordered Realtime Transitions

Use a small FIFO container for pending physical transitions. Every
`pressLane()` or `releaseLane()` appends one transition; every applied logical
callback consumes the oldest transition and adds its replay-control identity.
Existing publish-state deduplication remains unchanged. This makes the
reversal's release callback consume the release and the press callback consume
the press in the same order produced by the adapter.

### Startup Reservation Recovery

Run recovery only when opening and adopting a new replay database connection,
before callers can reserve new files. Under one `BEGIN IMMEDIATE` transaction:

1. Enumerate reservation rows that have no chart or course result with the
   same attempt ID.
2. Reconstruct the canonical relative path from each row's stem and history
   index. Retain malformed rows conservatively.
3. Inspect the canonical final path. Retain any present entry or any path that
   cannot be inspected safely; delete only a definitely missing final file.
4. Rebuild `replay_stem_sequences` from the maximum indexes in `replay_files`
   and the retained reservations.
5. Commit both reclamation and sequence recomputation atomically.

An installed final file is retained even if result staging did not finish,
because a later allocation must not overwrite it. Runtime retries within an
open session continue to reuse their existing reservation.

### Fixture Correction

The fixture generator derives the lane order using the pinned Beatoraja
modifier algorithms rather than declaring an unrelated literal. The
committed BRD fixtures are updated with deterministic gzip headers so their
checksums and decoded fields remain reproducible.

## Error Handling

- Stock decode continues to reject invalid gauge and key-mode values before
  deriving the starting percentage.
- An unmatched applied callback leaves the queue unchanged only when it is
  already empty; normal input behavior remains fail-closed.
- Reservation recovery never deletes a row when the final-path status is
  ambiguous, unsafe, or present.
- Any SQLite failure rolls back both reservation deletion and sequence
  rebuilding and prevents the candidate database session from being adopted.

## Test Strategy

1. Extend codec fixture tests to require a 100% HARD start and a permutation
   consistent with the stored RANDOM seed.
2. Add a realtime router regression that reverses clockwise to
   counter-clockwise while the first direction is held and requires ordered
   release/press worker transitions with the same timestamp.
3. Add repository tests that reopen the database, reuse indexes held only by
   abandoned fileless reservations, retain a reservation with an installed
   final BRD, and allocate after the retained maximum.
4. Run focused tests, the complete desktop suite, the desktop app build, and
   `scripts/ios_firebase_deploy.sh --build-only`.

## Acceptance Criteria

- Stock survival and course replay playback begins at the canonical gauge
  profile initial value.
- A native directional scratch reversal reaches the gameplay worker as an
  ordered backspin release followed by the new-direction press.
- Restarting a profile reclaims only fileless, resultless reservations and
  makes their trailing indexes allocatable again.
- Reservations with a final BRD remain protected from reuse.
- The stock fixture matches Beatoraja's option/seed reconstruction.
- Slot relocation and GitHub review-thread state remain unchanged.
