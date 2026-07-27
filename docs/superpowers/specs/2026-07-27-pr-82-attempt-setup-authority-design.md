# PR 82 Attempt Setup Authority Design

## Scope

Address the three review findings created at 2026-07-27 00:59 UTC and remove
the repeated field-by-field replay-setup reconstruction that caused the same
class of defect in retry, G-Battle, practice, ghost, and export branches.

The concrete findings are:

- schema-v10 migration silently skips malformed `random_values` tokens and can
  commit a different BMS `#RANDOM` branch;
- DP FLIP is recorded in BRD setup but omitted from result provenance and the
  result fingerprint, so a replacement BRD can disagree with its result;
- Chart Viewer materializes a FLIP replay, drops FLIP in the judged projection,
  then reparses the ghost as Normal.

The same lossy judged projection also reaches saved-result retry, G-Battle
retry, course video export, and result-image export. This design fixes the
authority boundary rather than patching each branch independently.

This work does not reply to or resolve GitHub review threads. It does not
restore replay reconstruction from result/provenance data, and it keeps raw
input events out of provenance and IR snapshots.

## Considered approaches

### Add DP to every partial model

Adding one field to `ScoreProvenance`, `JudgedPlaybackData`, retry options, and
the affected scenes is the smallest immediate patch. It leaves every bridge
responsible for copying an expanding list of setup fields, which is the cause
of the current regressions. A later setup field could be lost in exactly the
same way.

### Use provenance as the runtime replay model

Making `ScoreProvenance` the object passed through replay playback would make
saved-result binding direct. It would also couple raw BRD playback and
standalone Beatoraja imports to result eligibility, IR proof, judge-window
evidence, and other concerns that do not belong to a replay artifact.

### Use one captured attempt setup and one authority resolver

This is the selected approach. The existing `ChartPlaybackSetup` value already
describes the chart branch and gameplay choices needed to reproduce an
attempt; it is data, not encoded BRD bytes. A private resolved wrapper controls
how that value is created and consumed. BRD and provenance remain separate
persistence projections. A single resolver decides which projection is
authoritative and returns one resolved setup for all runtime consumers.

## Authority model

There are three explicit authority cases:

1. **New local attempt.** The resolved chart, `StartOptions`, and compiled
   gameplay policy produce one attempt-setup snapshot at play start. Result
   provenance and BRD setup are derived from that snapshot. Raw-only state,
   such as input transitions, touch samples, lane-cover events, and the exact
   starting gauge state, remains only in BRD/runtime data.
2. **BRD attached to a verified or modified saved result.** Completed-attempt
   provenance owns the semantic result facts. The resolver checks the decoded
   BRD against those facts. Aso-extension fields must agree exactly. Missing
   stock Beatoraja fields may be enriched only by the resolver under the
   existing stock compatibility rules. A mismatch fails closed.
3. **Standalone BRD or `LegacyUnverified` result.** The BRD owns playback setup.
   Provenance does not invent or override setup facts. This preserves native
   Beatoraja sharing and the existing legacy trust boundary.

`StartOptions`, judged playback, scene state, and export requests are consumer
projections. They are never independent authorities.

Move the two-value DP enum to a small neutral setup header so provenance can
record the fact without including BRD events, codec types, repository state,
or IR types. The existing replay namespace may retain a compatibility alias.

## Captured setup, resolved setup, and adapters

Do not perform a broad physical decomposition of `ChartPlaybackSetup` in this
PR. Wrap it in an immutable `ResolvedAttemptSetup` value with an origin such as
captured attempt, verified Aso extension, stock enriched from result, or legacy
replay owned. Construction is private to the setup authority. The setup covers:

- chart MD5/SHA-256, key mode, effective long-note mode, and BMS RANDOM
  seed/PRNG/value sequence;
- player-one and player-two play options and seeds;
- per-stage DP Normal/FLIP;
- assist and gauge setup;
- ruleset, playback rate/mode, candidate selection, judge-window scale,
  starting gauge percentage, and club mode.

At play start, one capture helper populates the semantic fields from the
resolved chart, launch options, and compiled policy. Provenance is projected
from this captured setup. BRD recording starts with the same setup and then
adds raw-only fields such as undefined-LN path state, exact gauge snapshots,
and lane-cover state. Judged playback retains the resolved setup beside
judgement events instead of reconstructing setup from its own partial field
list.

Shared adapters perform all boundary conversions:

- play-start inputs to captured setup;
- captured setup to provenance and BRD setup;
- provenance plus decoded BRD to resolved setup;
- resolved setup to judged playback and `StartOptions`.

Callers may add presentation/result facts but do not copy individual replay
setup fields. Prepared charts and their resolved setup travel together across
materialization, ghost, practice, video, and result-image boundaries. If a
consumer reparses a chart, it applies the retained resolved setup, including
DP FLIP, exactly once.

## DP FLIP provenance and compatibility

DP FLIP is a stage fact because a course BRD contains setup per stage. Add an
optional DP value to `ScoreStageProvenance` and advance the provenance schema.
New non-legacy provenance always records Normal or Flip. Serialization,
canonical result fingerprints, course merging, and IR snapshot fingerprints
therefore bind the fact for new attempts.

Provenance from the previous schema decodes as **DP unknown**, never as Normal.
Old fingerprints remain verifiable through a narrowly versioned legacy
fingerprint path that omits the new field. Binding an old result skips only the
unknown DP comparison; every previously known field remains enforced. New
provenance with a missing or invalid DP fact is rejected.

The resolver is used both before a completed attempt is persisted and after a
BRD is loaded. This prevents a locally generated inconsistent result/BRD pair
and a later coherently substituted BRD from taking different validation paths.

## Legacy RANDOM migration

The retired schema-v10 writer stored SQL NULL for an empty RANDOM vector and
otherwise emitted canonical comma-separated decimal `int` values. Migration
uses one private, length-aware, fail-closed reader for that exact storage
contract:

```text
int ("," int)*
int := "0" | "-"? [1-9][0-9]*
```

Values must fit `int`. Empty TEXT, empty tokens, whitespace, plus signs,
leading zeroes, `-0`, malformed tokens, overflow, embedded NUL, and non-TEXT
non-NULL SQLite storage reject the whole migration. Parsing uses
`std::from_chars<int>` into a temporary vector and publishes only after every
token succeeds. SQL NULL alone maps to an empty vector.

Rejection happens before file finalization and schema cutover. The schema stays
at version 10, source bytes remain unchanged, and a repaired database can retry
the atomic migration.

## Regression coverage

Tests cover:

- canonical NULL and boundary-valued legacy RANDOM sequences;
- malformed, overflowed, noncanonical, wrong-storage-class, and embedded-NUL
  RANDOM values rejecting atomically and succeeding after repair;
- provenance schema round-trip for stage Normal/Flip and old-schema unknown;
- new result and course fingerprints changing when only DP changes, while old
  fingerprints remain readable;
- completed-attempt save and repository load rejecting result/BRD DP mismatch
  for both chart and course records;
- stock Beatoraja setup enrichment and `LegacyUnverified` BRD authority
  remaining unchanged;
- raw/materialized adapters retaining resolved DP setup;
- Chart Viewer saved ghost and practice launch preserving FLIP;
- saved-result retry and G-Battle retry preserving FLIP;
- course replay/video and result-image reparse paths preserving per-stage FLIP.

After focused tests pass, inspect every conversion to or from
`ChartPlaybackSetup`, `JudgedPlaybackData`, `StartOptions`, and
`ScoreProvenance` in the full branch diff. Final verification includes the full
desktop test suite, desktop `main`, `git diff --check`, and the repository iOS
build-only script.
