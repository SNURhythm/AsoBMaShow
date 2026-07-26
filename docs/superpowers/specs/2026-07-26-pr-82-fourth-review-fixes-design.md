# PR #82 Fourth Review Fixes Design

## Goal

Address nine verified replay review findings without reintroducing database-
resident raw replay events, coupling replay files back to IR provenance, or
implementing the separately deferred Beatoraja slot-copy UI.

## Scope

This pass fixes:

1. Aso-only manual lane-assignment replays being rejected by BRD encoding.
2. Schema-10 all-miss migrations fabricating stock input transitions.
3. Compact result-summary limits being consumed by corrupt rows.
4. Saved course recall losing the persisted final gauge.
5. Failed course recall losing the persisted total stage count.
6. Migration assuming every legacy chart has undefined long notes.
7. Raw replay playback accepting a different chart at the recorded path.
8. Overlapping directional and digital scratch bindings producing unbalanced
   recorded transitions.
9. The result browser hiding Delete for corrupt regular replay files.

The production slot-selection and slot-relocation flow for Beatoraja history
slots remains deferred. No GitHub review thread will be replied to or resolved
as part of the local implementation.

## Chosen Approach

Make focused behavioral changes within the existing codec, migration,
playback-preparation, recall, input, repository, and result-capability
boundaries. Extract only small helpers whose contracts are exercised by more
than one branch. Do not introduce a broad replay-normalization layer or mix
unrelated refactoring into the fixes.

Two alternatives were rejected:

- A centralized replay-normalization subsystem would enlarge the change and
  force unrelated consumers through a new abstraction.
- Independent inline patches would duplicate identity rules, migration
  metadata handling, or bounded corruption scanning.

## Manual Lane Assignment in BRD

Beatoraja's stock replay option fields have no representation for AsoBMaShow's
`ASSIGN:<notation>` option. AsoBMaShow will retain the exact option and original
logical input transitions in the `asobmashow` extension.

For the stock Beatoraja view:

- validate the assignment against the replay key mode;
- derive the assignment's destination-to-source physical lane mapping;
- encode both stock option fields as `NORMAL` where the corresponding Aso
  option is a manual assignment;
- remap only the stock `keyinput` transitions from assigned destination lanes
  back to the original source lanes;
- convert lane-versus-scratch control kind as required by the source physical
  lane, using the existing best-effort clockwise stock scratch direction when
  an assigned key maps back to a scratch lane; and
- leave the extension input and setup unchanged.

Consequently, AsoBMaShow reproduces the exact assigned layout. Beatoraja uses
the original chart layout but reproduces the recorded transition timing and
judgement targets. Invalid or non-bijective assignment notation continues to
fail closed rather than producing misleading stock input.

An empty input stream is valid. Encoding and schema-10 migration must preserve
it as an empty compressed stock `keyinput`; migration must not insert a fake
lane press and release.

## Migration Chart Metadata

Replace the key-mode-only migration resolver result with one coherent chart
metadata result containing:

- the supported key mode; and
- whether the chart actually has undefined long notes, defined as a positive
  long-note/backspin count with chart metadata `ln_mode == 0`.

The chart database lookup continues to match exact normalized SHA-256 first,
MD5-only legacy identity when applicable, and path only when no digest exists.
Conflicting matches, unsupported key modes, an unavailable chart database, or
failed reads return no resolved metadata.

When metadata resolves, the migrated replay setup and canonical BRD stem use
both returned facts. When it does not resolve, key mode retains the existing
event-based fallback and undefined-LN status retains the legacy conservative
fallback. Course migration aggregates the corrected per-stage undefined-LN
facts instead of assuming every stage is undefined.

## Replay Chart Identity Gate

Raw replay preparation must compare the parsed chart's MD5/SHA-256 with the
stored replay setup before applying randomization or manual lane assignment.
Use the existing legacy identity rule so a schema-10 MD5-only fallback SHA-256
continues to match the real chart by MD5.

A mismatched or malformed identity returns no prepared chart. This fail-closed
result propagates through Watch Replay, G-Battle materialization, analysis,
and video export instead of applying recorded input to different chart data.

## Saved Course Recall

Reconstructed saved-course sessions must retain the aggregate facts that the
result presentation consumes:

- initialize `carriedGauge` from the persisted course gauge configuration and
  `finalGauge`;
- retain the persisted course clear rank in an explicitly saved-result-only
  session field so aggregate presentation does not recompute a different lamp;
  and
- make `entries.size()` equal persisted `totalCharts`, appending presentation-
  only empty entries after the completed-stage prefix.

Only `result.stages` are parsed into owned charts and completed results. Empty
entries cannot be navigated as saved stages because saved-result continuation
remains bounded by `completedResults.size()`. They exist solely so course
metadata, missing-stage gauge history, completion checks, and full-combo
checks retain the recorded denominator.

The aggregate recalled result restores the saved-result-only clear-rank
override after aggregating completed stages. It must not derive a different
final gauge or clear presentation from a freshly configured default gauge.

## Scratch Handoff Recording

Directional scratch state and digital scratch lane state may overlap while
sharing one physical held lane. When logical ownership changes without a
physical release, the adapter will emit balanced replay notifications:

- release the previously recorded logical scratch control; then
- press the newly owning logical scratch control at the same transition.

The adapter must not call the rhythm control's physical release/press pair for
such a handoff. The lane remains continuously held for gameplay, while the
recorder observes matched direction-specific transitions. The behavior must
work for directional-to-digital and digital-to-directional ownership changes,
including counter-clockwise directional input.

## Bounded Summary Scanning

Positive-limit compact chart and course summary reads will treat the requested
limit as the number of valid summaries returned, not the number of candidate
IDs inspected.

Scan ordered IDs in bounded chunks until one of these conditions holds:

- the requested number of valid summaries has been collected;
- no more candidate rows exist; or
- the existing `kCorruptCandidateAllowance` has been exhausted.

`limit <= 0` remains the explicit unbounded mode. Ordering remains newest
first. If database stepping fails, fail closed as today. Corrupt candidates do
not enter the result, and scanning never exceeds the requested positive limit
plus the corruption allowance.

## Corrupt Replay Deletion

For a non-autoplay local result, `ReplayFileState::Corrupt` enables only the
Delete Replay File capability among file-consuming actions. Watch, G-Battle,
video export, and sharing remain disabled. Missing, unsafe, and I/O-failed
states remain non-deletable through this capability.

The existing deletion service remains authoritative: it re-inspects the file,
accepts Available or Corrupt regular files, deletes the file, and retains the
score/result/database reference.

## Error Handling

- Invalid manual assignment, invalid remapping, and malformed replay identity
  fail encoding or preparation without partial output.
- Missing or ambiguous migration metadata uses the documented legacy fallback
  and does not make profile initialization dependent on chart database
  availability.
- Course recall continues to reject missing or identity-mismatched completed
  stage charts without publishing a partial session.
- Summary corruption scanning is bounded and preserves existing fail-closed
  behavior for storage errors.
- Scratch handoffs change logical recorder notifications only; physical input
  continuity remains unchanged.

## Test Strategy

Each production change follows a focused red-green cycle.

- Codec tests cover manual assignment extension round-trip, `NORMAL` stock
  fields, literal remapped stock key records, invalid assignment rejection,
  and valid empty input.
- Migration tests cover empty all-miss input, defined-LN canonical stems,
  undefined-LN prefixes, course aggregation, and missing/ambiguous metadata
  fallback.
- Replay preparation tests cover matching SHA-256, legacy MD5-only identity,
  and mismatched chart rejection through the real helper.
- Course recall tests cover persisted final gauge restoration and incomplete
  course total-stage preservation without exposing placeholder stages for
  navigation.
- Logical input tests assert physical lane continuity and balanced applied
  replay transitions for both scratch handoff directions.
- Repository integration tests place corrupt rows ahead of valid chart and
  course rows, verify the requested number of valid summaries is returned,
  and verify the corruption allowance remains bounded.
- Result capability tests verify corrupt files expose Delete but no playback
  or sharing actions.

After focused suites pass, run the complete desktop CTest suite, rebuild the
desktop `main` target, and run `scripts/ios_firebase_deploy.sh --build-only`.
The iOS command must not archive, sign, or upload a build.

## Review-Thread Disposition

Address all nine findings listed in Scope. Leave the Beatoraja slot-copy UI
and occupied-slot relocation threads open and deferred. Do not reply to or
resolve any GitHub review thread without explicit authorization.
