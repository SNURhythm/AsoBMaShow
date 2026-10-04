Candidate differential oracle
=============================

Run from the repository root:

```sh
python3 scripts/compare_lr2oraja_candidates.py
```

The reference is the sibling `lr2oraja-endlessdream` checkout at commit
`5233be081abee2a7f824b78aed0d783847deeb2f`. `--reference-repo` accepts another
checkout of that commit. The script verifies each reference source against the
pinned Git object and prints SHA-256 hashes for `JudgeManager.java`,
`JudgeProperty.java`, and `JudgeAlgorithm.java`.

The Java probe compiles JudgeProperty and JudgeAlgorithm unchanged. It extracts
the candidate scanner and MultiBadCollector directly from JudgeManager. The only
adaptations replace lane iteration with an array and suppress a diagnostic print;
small Note objects provide timestamp, resolved state, and normal/LN type.

The native probe compiles the production GameplayJudgeRules,
CompiledGameplayJudge, and GameplayCandidateRules implementations. It compares
selected identity, final judgement, and every ordered multi-BAD identity.

Coverage: 57,664 cases. Every sorted two-note combination at normal-rank tier
edges and the outer scan limits is tested across normal/LN type, unplayed/played
state, and Lowest/Combo/Duration/Score. An additional 10,000 deterministic random
clusters of one to seven notes are each tested with all four algorithms, using
extended ranks from 1 to 400 and equal custom window rates from 0 to 200.

Final observed result: 57,664 comparisons, zero mismatches. Standalone regression
checks were observed failing before the fix for rejected late-BAD LN replacement
and loss of configured Lowest. Native simulation tests exercise the real chart
scanner and legacy input path. This oracle does not cover parser construction,
multiple input events, key binding, HCN cadence, audio, or UI presentation.

Separate realtime worker regressions cover simultaneous lane order and repeated
same-key edges. LR2 judges the final changed state for each key in a collected
equal-time batch, matching JudgeManager's key-state snapshot; accepted replay
history retains every edge. Directional scratch controls identify clockwise and
counterclockwise keys separately. Legacy Independent scratch events without a
logical direction cannot identify their physical key and retain event order.
Known scratch directions use shared simulation methods that retain the key
which started a long note: an opposite key completes CN/HCN and recovers a
classic LN release. Raw-key reference comparisons also check early BAD releases
that remain pending until the update's input phase has finished, including a
zero release margin and the resulting FAST/SLOW timing counts.

The same command independently compares 72,000 timing-window rows across all
four roles (normal, scratch, LN end, scratch LN end), every extended rank 1–400,
and window rates 0, 1, 33, 50, 75, 99, 100, 101, 125, and 200. `WindowProbe.java`
executes the unchanged JudgeProperty; `window_probe.cpp` compiles the native
rules. The final observed timing-window comparison also reports zero mismatches.
