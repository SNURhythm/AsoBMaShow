# PR 82 Eighth Review Fixes Design

## Scope

Address the eight unresolved review findings on PR 82 without restoring replay-event rows to SQLite, coupling IR provenance back to replay input, or changing Beatoraja's replay path layout.

## Selected design

### Gauge fidelity

Raw BRD setup metadata gains an optional initial `GaugeStateSnapshot`. AsoBMaShow writes it into the existing optional `asobmashow` extension and restores it for playback and materialization; stock Beatoraja readers continue to ignore the extension. Older BRDs and stock BRDs retain the scalar `startingGaugePercent` fallback.

Compact results gain an explicit adopted/final gauge type for chart results and course stages. Replay schema 12 adds checked integer columns for these values. Schema 11 profiles migrate in one SQLite transaction and infer the initial gauge type from provenance for records created before the field existed. New captures persist `state.gaugeType`, use it when recalling the adopted gauge history, and include it in result fingerprints.

Course raw replay materialization carries the previous stage's complete `GaugeStateSnapshot` together with combo state. This matches live course gameplay and the judged-playback export path.

### Migration integrity

The chart metadata resolver also returns authoritative `total_notes`. When a schema-10 replay no longer has a pending score row, migration derives `maxScore` from `total_notes * 2`; if neither pending result facts nor authoritative metadata are available, migration fails before cutover rather than inventing a truncated maximum.

Legacy event actions, touch actions, judgements, and event gauge types are validated before conversion. Any unknown enum fails the atomic migration and leaves schema 10 authoritative.

### Reservation recovery

Startup recovery treats every reservation without a chart/course result as abandoned because no in-memory retry survives process restart. It removes a safe regular finalized BRD first and then deletes the matching reservation in the SQLite transaction; missing files simply delete the reservation, while unsafe or unreadable paths remain untouched. The Result scene also attempts the same cleanup when the user chooses Continue Without Saving or leaves an unstaged result, so normal abandonment does not wait for restart.

### Records loading

Replay summaries start in an `Unchecked` file state. Opening Records performs no BRD reads or hashes. Selecting a local record inspects only that BRD, updates its summary/capabilities, and presents missing/corrupt/unsafe status. File actions continue to re-inspect immediately before use.

### IR snapshot sizing

The canonical IR snapshot read/write bound becomes 16 MiB, matching the compact result JSON bound and the supported one-million-sample gauge history. Provider payload limits remain unchanged; an optional provider may reject or omit an oversized upload draft without blocking local score/replay persistence.

## Failure behavior

- Unsupported or malformed new BRD gauge snapshots reject the Aso extension rather than silently changing gauge behavior.
- Schema upgrades are transactional; failure leaves the previous schema usable for retry.
- Reservation cleanup removes only valid contained regular files owned by result-less reservations.
- Migration never deletes schema-10 source rows when metadata or enums are invalid.
- Records actions remain disabled until the selected local replay passes inspection.

## Verification

Use regression tests for raw gauge round trips/playback, adopted gauge recall, course materialization carry, schema 11-to-12 migration, schema-10 max score and enum rejection, reservation recovery/abandonment, lazy Records state, and snapshots over 256 KiB. Then run the complete desktop CTest suite and `scripts/ios_firebase_deploy.sh --build-only` without uploading.
