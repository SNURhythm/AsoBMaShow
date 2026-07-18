# Ranking Detail Accuracy Design

## Goal

Stop the Bokutachi ranking detail modal and local comparison banner from
presenting derived values as if they were authoritative score data.

## Root Cause

At Tachi commit `233bc992f74cd314c8ef9bc2730d714904838dfc`, the Beatoraja ranking
converter in
`typescript/server/src/server/router/ir/beatoraja/charts/_chartSHA256/convert-scores.ts`
always synthesizes the four timing fields from EX score:

```text
epg = floor(score / 2)
egr = score % 2
lpg = 0
lgr = 0
```

The converter calls this a temporary compatibility hack. AsoBMaShow currently
parses those fields as literal early/late PGREAT and GREAT counts, so a 1284 EX
score appears as 642 early PGREAT and no other judgements even when the real
play was 511 PGREAT and 262 GREAT.

The local comparison has an independent semantic error: it labels
`comboBreak` as BP. For LR2/Bokutachi, BP is `BAD + POOR + KPOOR`, while combo
break excludes KPOOR.

## Chosen Design

Keep using the Beatoraja-compatible ranking endpoint for the leaderboard, but
treat its four synthesized judgement fields only as an encoding of EX score.
The normalized ranking entry stores timing-split judgements as optional
values. The Tachi parser leaves them absent because this endpoint cannot prove
them. The detail presentation shows a single centered explanation when the
breakdown is absent instead of four fabricated metric cards.

Provider-neutral entries retain the ability to carry real optional timing
counts in the future. When all four values are present, the existing four-card
layout remains available.

Local comparisons use authoritative BP evidence:

- The result screen sums the current state's BAD, POOR, and KPOOR counts.
- Song select loads the selected local best row on demand and exposes the same
  sum from its stored judge counts.
- The existing score summary cache remains unchanged; the on-demand read only
  occurs when the user opens rankings.

This avoids a score-database schema migration and does not change ranking HTTP
requests, score submission, the durable outbox, or credentials.

The result screen's existing `BREAK` metric is deliberately unchanged. It
continues to mean combo breaks (`BAD + POOR`); the separate sum including
KPOOR is calculated only for fields explicitly labeled `BP` in the ranking
UI.

## Rejected Alternatives

Using the synthesized fields with different labels remains misleading because
they are only an EX-score decomposition. Enriching every row from Tachi's
native API requires chart-ID resolution, pagination, user mapping, and another
asynchronous detail flow; it is disproportionate and still cannot guarantee
early/late data for historical or composite PBs. Overlaying local data only on
the current-user row would make rival rows inconsistent and can associate one
local play with a composite Tachi PB incorrectly.

## Error Handling

Missing timing splits are valid normalized data, not a malformed response.
The parser continues validating the compatibility integers and using them to
compute EX score. Invalid counts still reject the complete response. Stored BP
is exposed only when its sum is nonnegative and fits in `int`; otherwise the
local comparison uses the existing em dash.

## Testing

- Parser regression: a row containing plausible timing splits produces the
  correct EX score but no authoritative normalized judgement breakdown.
- Modal regression: optional real counts format normally; absent counts show
  the unavailable state rather than zero or synthesized numbers.
- Repository regression: a stored score with BAD 14, POOR 8, and KPOOR 40
  loads BP 62.
- Result and main-menu source/behavior tests verify they feed the corrected BP
  into `IrLocalComparison`.
- Build the desktop target and run the complete CTest suite.
