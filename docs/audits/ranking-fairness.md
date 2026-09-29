# Ranking fairness audit

## Reference revisions

- LR2: `../lr2oraja-endlessdream`, `5233be081abee2a7f824b78aed0d783847deeb2f`.
- Beatoraja: `../beatoraja`, `c2ed5db1a46145ed10790c3872f717e95b59db9d`.
- Parser dependency: `../bms-parser-cpp`; changes must be tested and amalgamated there.

The two gameplay rulesets have separate references. LR2-specific multi-BAD and
late-BAD exclusion for long-note heads must not be applied to Beatoraja.
Reference Java timing differences are `note - input`; this application stores
`input - note`, so asymmetric window edges must be negated and exchanged.

## Corrections

| Area | Corrected behavior |
| --- | --- |
| Chart rank | Missing RANK defaults to NORMAL; strict RANK validation and DEFEXRANK type/value survive parsing, chart caches, policy capture, and replay validation. |
| Timing tables | Beatoraja BAD scaling, scratch/tail contexts, rank 4 and mode-specific tables; LR2 tail rank scaling; integer truncation and extended-rank interpolation. |
| Miss scheduling | Scratch misses use their own BAD deadline, including when deadline order differs from note order. Paired CN head/tail misses commit before survival failure is latched. |
| Ordinary LN | Combine judgement severity independently of the larger head/tail timing error; held-through-tail completion preserves the accepted head. |
| CN / scratch CN | Out-of-window release is POOR; scratch lift within tail windows waits for reversal or automatic miss. |
| HCN | Continuous gauge updates start only after head judgement; a successful early tail continues gaining until its nominal endpoint. |
| Candidate selection | Played notes can receive empty POOR only in the relevant ruleset window; retain the closer empty-POOR candidate; scratch selection uses scratch windows; late-LN BAD candidates may contribute LR2 multi-BAD without being selectable. |
| Multi-BAD | A CN/HCN head does not silently consume its independently scored tail. |
| PMS | BAD leaves the identity recoverable; single-miss suppression consumes a missed identity without a second penalty; an early LN release has a 200 ms recovery margin. Replay counters and visuals follow the same rules. |
| Mines | Resolve after all input edges at the authored timestamp, consistently for live input, autoplay, and replay; publish quiet mine expiry to presentation. |
| Gauge | Correct modifier conditions, Java float evaluation order, profile-specific guts/death thresholds, and automatic-shift behavior against each reference. |
| Counts | Empty POOR does not advance passed-note count; exact hits belong to FAST; mode-specific combo behavior and total FAST/SLOW definitions. |
| Bad points | Persist the exact failed-attempt total, including unpassed notes and PMS nonvanishing BAD. Course totals include unattempted future charts. Preserve historical result fingerprints and displays. |
| Authority | Manual macOS/Android input reaches the audited simulation through the existing input bridge. Any live fallback judgement or mine damage invalidates verified provenance. |
| Provenance | Bump behavior versions, preserve historical descriptors, reject stale verification, and rebuild derived best-score caches without rewriting historical fingerprints. |
| Replay compatibility | Label prior algorithms obsolete, preserve results and original replay bytes, and disable actions that would reinterpret old input using new rules. |

## Migration policy

This unreleased audit groups its changes into LR2 ruleset version 4, Beatoraja
ruleset version 3, chart database version 12, score database version 14, and
replay database version 19. Each database retains its independent version scheme.
Replay database 19 adds nullable bad-point facts; existing rows remain NULL.
Score database 14 also adds nullable course bad-point totals and rebuilds the
derived best-score caches against current supported ruleset descriptors.
Historical result serialization omits absent new facts, preserving fingerprints.
When the duration backfill waits for a chart rescan, a transactional completion
marker records the independent version-14 work so repeated lookups do not
rebuild score summaries. The duration migration still retries after the scan.

Old input cannot generally be migrated faithfully after candidate selection,
long-note handling, and scoring changes: running it under the corrected algorithm
can change the recorded outcome. Its original ruleset descriptor therefore stays
unchanged and its replay becomes obsolete. Saved results and byte-preserving
replay actions remain available. Compatible metadata normalization does not
promote an old algorithm version to a current one.

Parser source changes are committed in `bms-parser-cpp` at `827d360`; the two
parser files in this repository are generated amalgamations from that revision.

Mines use an explicit deterministic convention: their decision is scheduled one
microsecond after the authored timestamp so all equal-time input edges settle
first. The Java reference samples current lane state within a frame and does
not define a microsecond tie order. Live simulation and replay materialization
share the same convention here.

Extended-rank replay validation permits the canonical window magnitude derived
from the captured rank percentage. Snapshots without an extended rank retain the
existing two-second limit. This avoids rejecting valid large DEFEXRANK charts
during result serialization or replay setup.

The candidate scanner's far-future cutoff is exclusive even though individual
judge windows are inclusive. Both reference `JudgeManager` implementations stop
at `noteTime - inputTime >= mjudgeend` before consulting the window table. Thus an
LR2 press exactly 1,000,000 microseconds early (Beatoraja: 500,000) produces no
judgement; one microsecond inside can produce empty POOR. A regression covers
both authorities, normal/scratch lanes, all five ranks, and adjacent timestamps.

Review regressions also cover recovery inputs that do not retrigger a keysound,
immediate replay-capacity failure on replay-only release/recovery transactions,
and obsolete IR actions independently of replay-file availability. Partial course
adapters retain saved facts for every entry so exports, replay restarts, and saved
result browsing include unplayed notes; course images preserve the final stage's
judgement difficulty.

The whole-branch regression review also corrected replay reconstruction after a
recovered PMS long-note head BAD and video cutoffs for gauges whose automatic
shift policy keeps play running. Historical partial-course browsing retains its
original observed BP when exact passed-note facts were not saved. Chart metadata
rebuilds preserve chart and folder added dates across interruptions and archive
replacement; only a complete configured-library scan removes that restoration
state. An eligibility index lets attached score queries recognize legitimate
empty historical caches without repeatedly rebuilding or scanning old scores.
Result-skin BP record and tie flags now share the exact BP calculation used by
the displayed number, including notes left unplayed after an early failure.

## Primary source locations

Both reference checkouts define these rules under `bms/player/beatoraja/play/`:
`JudgeProperty.java`, `JudgeAlgorithm.java`, `JudgeManager.java`,
`GaugeProperty.java`, `GrooveGauge.java`, and `BMSPlayerRule.java`.
The LR2 checkout prefixes this with `core/src/`; Beatoraja uses `src/`.

## Verification

Focused regressions cover source-derived timing boundaries, LN/CN/HCN and PMS
sequences, mine input ordering, optimized float arithmetic, exact BP round trips,
historical fingerprints, obsolete replay capabilities, and migration rollback.
Independent read-only review found no outstanding source blockers.

The build includes `fix/optimize-build-artifacts` through `ab1c1377`, which shares
compatible test objects and adds two build-configuration checks.

The final desktop and test build passed with
`cmake --build cmake-build-debug --target main all -j 6`.
The complete parallel CTest run passed 402 of 404 entries in 346.41 seconds.
The legacy-profile fixture initially retained the new eligibility index while
dropping its referenced columns; removing that modern index restores the old
schema fixture, and all four profile archive suites then passed together.
The audio renderer exceeded its unchanged 30-second CTest timeout in both the
parallel run and an isolated rerun, then passed directly in 21.10 seconds.
Every test entry therefore has a passing result across these runs; the full
parallel run itself was not completely green. The previously observed audio
timing sensitivity remains a verification limitation.

All seven focused regression suites passed, as did the focused result-skin BP
comparison cases and the full skin session test. Each reviewed defect was first
reproduced by a failing permanent regression. Three fresh independent reviewers
covered gameplay/builds, persistence, and replay/UI; follow-up reviews of the
final BP correction and legacy fixture found no remaining regressions.

Parser source verification passed `make clean`, `make test`, and
`make test_amalgamation`; both copied parser files match that generated output.
No manual release or deployment was performed. The feature commits are development
checkpoints; the final series tip is the verified behavior and migration boundary.
