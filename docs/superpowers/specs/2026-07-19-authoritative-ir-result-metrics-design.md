# Authoritative IR Result Metrics Design

## Problem

`makeIrSubmission()` currently reconstructs judgement timing from replay events
even though gameplay has already decided which events contribute to the saved
result. Classic LR2 long-note heads expose why this is unsafe: their replay
events retain informational judgements for playback, while only the final
long-note judgement is committed to `RhythmState`. The EX-score-delta heuristic
can distinguish PGREAT and GREAT heads, but GOOD, BAD, and POOR do not change EX
score. Their informational heads are therefore double-counted and the complete
Bokutachi early/late breakdown is discarded.

This is the second failure caused by independently interpreting result semantics
inside the IR projection. Extending that interpretation with another replay
heuristic would leave multiple authorities and more judgement-specific gaps.

## Decision

The completed `RhythmState` is the only authority for result metrics. When
`makeChartResultAttempt()` freezes a completed result, it will snapshot:

- judgement totals and aggregate FAST/SLOW in `ChartScoreWrite`;
- per-judgement fast/slow counters in a new `ChartJudgementTiming` value; and
- the adopted gauge's history in `adoptedGaugeHistory`.

`makeIrSubmission()` will only validate and map those captured values. It will
not derive judgement timing or gauge history from replay events. Replay remains
the durable input used to reconstruct `RhythmState` for historical result
recall, but the chart-aware reconstruction runs before the result-attempt
boundary. This preserves one result-semantic implementation for both live and
recalled scores.

The earlier `2026-07-18-ir-score-contributing-timing-design.md` EX-score-delta
decision is superseded by this design.

## Data Model and Mapping

`ChartResultAttempt` gains an optional `ChartJudgementTiming` snapshot containing
the `JudgementFastSlowCount` for every judgement. The factory always supplies
it. The optional form keeps manually constructed and legacy test attempts safe:
absence means optional Bokutachi timing fields are unavailable, never inferred.

For each of PGREAT, GREAT, GOOD, BAD, and POOR:

- `late` is the authoritative `slow` count (`diffMicros > 0`);
- `early` is the saved judgement total minus `late`, preserving the existing
  convention that exact-zero judgements belong to the early side;
- PGREAT `fast` and `slow` are copied directly for Bokutachi's aggregate
  FAST/SLOW exclusion.

Before mapping, the IR boundary validates that all per-judgement fast/slow
counters are nonnegative, neither pair exceeds its judgement total, KPOOR and
NONE contain no timing, and the sums equal the saved aggregate FAST/SLOW
counters. Present but inconsistent evidence invalidates the submission instead
of silently fabricating or dropping details.

`adoptedGaugeHistory` is likewise the only submitted gauge history. It remains
optional for legacy attempts, but there is no replay fallback.

## Compatibility and Durability

No score-database migration is needed. The timing snapshot exists while the
canonical outbox payload is built; the outbox already durably stores that
payload. Historical Records recall reconstructs the chart-aware `RhythmState`
from the replay and then calls the same attempt factory, so recalled uploads use
the same authority as live uploads. Existing outbox rows stay immutable.

## Tests

- Verify the attempt factory captures per-judgement timing from `RhythmState`.
- Regress a classic-LN replay containing duplicate informational GOOD and BAD
  heads and confirm the IR output follows only the captured result snapshot.
- Verify replay gauge/timing contents cannot override captured result metrics.
- Verify missing evidence omits optional timing and inconsistent evidence is
  rejected.
- Run focused model/IR tests, the full CTest suite, and the desktop build.
