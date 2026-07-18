# IR Score-Contributing Timing Design

## Problem

LR2 classic long notes record a judged replay event at the head so replay
playback can preserve the accepted head judgement. That head does not commit a
score judgement. The long-note release records the final applied judgement and
does commit it. `makeIrSubmission()` currently counts both events when deriving
the Bokutachi early/late PGREAT and GREAT breakdown. Its completeness guard then
detects that the replay counts disagree with the result counts and omits
`epg`, `lpg`, `egr`, and `lgr`.

## Decision

Use the cumulative EX-score snapshot already stored on every `ReplayEvent` to
identify score-contributing PGREAT and GREAT judgements. While traversing replay
events in order, a PGREAT contributes timing evidence only when its event score
advances the observed cumulative score by exactly 2; a GREAT contributes only
when it advances it by exactly 1. All events continue to advance the observed
cumulative-score position when their stored score is greater.

This preserves the informational long-note-head judgement for replay playback,
does not require a replay schema or database migration, and excludes classic
long-note heads because their score snapshot is unchanged. Modern live attempts
already populate the score snapshot after each committed judgement. Legacy
replays without trustworthy score snapshots remain safely unavailable rather
than receiving inferred timing data.

## Data Flow

`makeIrSubmission()` will keep gauge-history collection independent from timing
collection. Gauge-mutating events are validated and collected exactly as they
are today. PGREAT fast/slow subtraction and the four judgement-timing counters
will use only score-contributing replay events. The existing completeness check
against `ChartScoreWrite::pGreat` and `great` remains the final authenticity
guard. `TachiBatchManual` continues to emit all four fields together only when
that guard succeeds.

## Edge Cases

- Unjudged input, mines, gauge ticks, GOOD/BAD/POOR/KPOOR, and classic long-note
  heads do not provide PGREAT/GREAT timing evidence.
- Repeated or stale cumulative score snapshots do not contribute evidence.
- A malformed jump that does not match the current PGREAT/GREAT value is not
  attributed to that event; the completeness check keeps the breakdown absent.
- Gauge history and its adopted-gauge selection are unaffected.
- Existing stored submissions are immutable; the fix applies to newly built IR
  submissions and does not fabricate fields for PBs already stored remotely.

## Tests

Add a focused `ir_driver_tests` regression containing a classic long-note head
PGREAT with an unchanged score followed by a release PGREAT that advances score
by 2. Assert that only the release contributes to timing and PGREAT fast/slow
evidence, while gauge history still retains both mutations. Update existing
timing fixtures to provide realistic cumulative score snapshots, and run the
focused test followed by the complete CTest suite and desktop build.
