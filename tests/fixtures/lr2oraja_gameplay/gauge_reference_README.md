# Executable gauge comparison

Run from the repository root:

```sh
python3 scripts/compare_lr2oraja_gauges.py
```

The script reads the sibling `lr2oraja-endlessdream` checkout, requires commit
`5233be081abee2a7f824b78aed0d783847deeb2f`, and rejects uncommitted changes to the
three source classes it consumes. `--reference`, `--cxx`, and `--java-home` allow
other local paths. `--allow-unpinned` is for investigating a different reference
revision; the output reports the revision actually used. Builds and generated
input/output stay in a temporary directory and are removed afterward.

The Java probe executes the actual `BMSPlayerRule.validate`, `GaugeProperty`,
`GrooveGauge`, and nested `Gauge` implementations. The support JSON supplies only
model storage, type declarations, and unused dependencies. Its dummy judge-rank
window values do not participate in gauge arithmetic. No reference gauge or
TOTAL formula is duplicated in the harness. The C++ probe executes the actual
`compileGameplayGaugeRules` and `GameplayScoreState` implementation.

The comparison checks exact IEEE-754 bits for effective TOTAL, each gauge's
initial/minimum/maximum/border/death/base-delta properties, and every post-action
gauge value; it also checks qualification after every action. Coverage includes:

- Standard LR2, default LR2 courses, explicit 5/7/9/24/LR2 course constraints,
  and named StandardLr2/CourseLR2 profiles under the Beatoraja ruleset.
- All nine standard gauges and all three selectable course gauge classes.
- Six judgements, rates 0.3/0.5/0.7/1/1.3, repeated and mixed sequences, mines,
  starting clamps, irreversible death, and qualification borders.
- Fractional positive TOTAL, nonpositive/missing TOTAL, all damage TOTAL
  boundaries, every integer note count 1 through 1001 for damage arithmetic,
  additional default-TOTAL boundaries, and charts with 3000 notes.

A successful run currently reports **220820 cases, 0 mismatches**. Each mixed
case compares 500 successive actions, so a case is more than one value check.
A zero result establishes parity for this matrix. It does not claim every
possible floating-point input was enumerated. The separate upstream Beatoraja
profiles, BMSON inputs, candidate selection, HCN update cadence, auto-shift
selection, replay restoration, and UI rendering are outside this gauge probe.
