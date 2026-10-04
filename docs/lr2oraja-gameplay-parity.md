# LR2oraja gauge and judgement comparison

Reference: `lr2oraja-endlessdream` commit
`5233be081abee2a7f824b78aed0d783847deeb2f`.

The LR2 ruleset follows this fork. The separately selectable Beatoraja ruleset
keeps its upstream behavior; its explicitly named LR2 gauge profiles share the
corrected LR2 gauge calculations.

## Corrections

- Preserve fractional TOTAL and Java's floating-point evaluation order, including
  default TOTAL and damage calculations. Apply explicit course gauge constraints,
  grade damage reduction, recovery rates, clamps, and survival death thresholds.
- Follow reference candidate replacement, rejected long-note handling, multi-BAD
  ordering, and the selected Combo/Lowest/Duration/Score algorithm. Replay policy
  retains its recorded priority independently of current settings.
- Process incoming physical states, passing notes and HCN ticks before judgement
  input, then automatic misses. HCN applies at most one tick per update and keeps
  the remainder. Classic held long notes finish strictly after the tail time.
- Treat simultaneous input as one update in lane order. Scratch CN/HCN reversals
  finish the held tail without also hitting a following note.
- Rejudge recognized older replay formats under current rules. Preserve saved
  provenance and result records, and label a reproducible replay whose judged
  result differs as stale. File integrity and chart/setup identity checks remain
  required.

LR2 ruleset version is now 5; Beatoraja version is 4 because its named LR2 gauge
profiles changed. Historical LR2 versions 3/4 and Beatoraja versions 2/3 are
recognized for input rejudging with their original model identifiers.

Previously verified LR2 v4 results and frozen IR submissions remain eligible
under their original v4 proof, including the Records upload action. This keeps
existing IR candidates available without changing their historical scores.
Eligibility still checks the exact historical model identity,
canonical v4 windows, candidate policy, and TOTAL calculation; v3 and unknown or
modified proofs remain unsupported. New plays carry v5. Replay rejudging never
rewrites a saved score or upgrades its IR proof.

## Repeating the comparison

From the repository root with the pinned reference checkout alongside it:

```sh
python3 scripts/compare_lr2oraja_gauges.py
python3 scripts/compare_lr2oraja_candidates.py
python3 scripts/compare_lr2oraja_runtime.py
```

Each script verifies the reference source and compiles the production native
implementation into a temporary directory. The runtime script also requires the
existing desktop test build's parser object and localization library. Detailed
probe construction and coverage are documented in
`tests/fixtures/lr2oraja_gameplay/`.

The gauge probe executes the actual Java gauge classes and compares exact float
bits. The candidate probe executes the actual window/algorithm classes and the
extracted reference scanner. The runtime probe executes the full unchanged Java
JudgeManager with chart/input/host doubles; it checks judgement counts, combos,
mine hits, and HCN tick sequences. Its host gauge is a test double, so gauge
arithmetic is established by the separate gauge probe.

Final differential coverage:

| Matrix | Comparisons | Mismatches |
| --- | ---: | ---: |
| Gauge arithmetic and properties | 220,820 | 0 |
| Candidate selection and multi-BAD | 57,664 | 0 |
| Timing-window rows | 72,000 | 0 |
| Runtime counts, combo, FAST/SLOW, mines, and HCN ticks | 1,348 | 0 |

These executable matrices establish parity for their covered inputs, not an
exhaustive proof over every possible chart and update schedule. HCN depends on
update cadence in the reference. Replay rejudging uses reproducible input and
reports stale results instead of recording historical frame scheduling.

The final desktop build and all 420 CTest tests pass. This includes regression
coverage for stale replay playback without saved-fact mutation, original v4 IR
proofs and Records actions, and the legacy practice HCN initial passing bound.
