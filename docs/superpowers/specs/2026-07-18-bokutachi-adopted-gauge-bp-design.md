# GAS Gauge History and Bokutachi BP Design

## Goal

Capture complete histories for every gauge tracked by Gauge Auto Shift (GAS),
make the result graph selectable by gauge, submit only the final adopted gauge
history to Bokutachi, and include KPOOR in BP.

## Canonical gauge histories

`GameplayScoreState` retains the existing active-gauge `gaugeHistory` for
compatibility and adds one history per gauge type. On every gauge mutation it
records the post-mutation value for every gauge being tracked by the active GAS
mode. Without GAS, only the active gauge series is recorded.

Bounded realtime gameplay reserves each series before play. All series share
the existing logical sample capacity, so reaching the limit still produces the
same gauge-history integrity failure without allocating on the realtime
thread. The realtime worker transfers the full set of histories when gameplay
stops. Replay result reconstruction recomputes the same parallel histories and
then applies the recorded active-gauge snapshot for bit-exact compatibility.

## Result graph

The result graph initially displays the complete history for the final active
gauge, which is the gauge adopted by GAS at the end of the play. When more than
one relevant GAS series is available, each tap advances to the next series and
wraps back to the first. A label inside the graph shows the selected gauge type
using the corresponding clear-lamp color. Non-GAS results remain a one-series
graph.

Relevant series follow the configured mode:

- Best Clear and Select-to-Under expose their admitted gauge range.
- Survival-to-Groove exposes the selected survival gauge and Normal gauge.
- Other modes expose only the final active gauge.

## Gauge history

`makeChartResultAttempt` snapshots the final gauge's complete series from the
score state into the transient attempt. `makeIrSubmission` uses that series as
`IrSubmission::gaugeHistory`. This removes values from other gauges used during
GAS transitions without truncating the adopted gauge's history to the point at
which it became active.

For legacy or synthetic attempts that do not contain the state-derived series,
IR construction falls back to gauge-mutating replay events whose `gaugeType`
matches the final gauge-mutating event. Every gauge-mutating replay value is
still validated, and PGREAT early/late derivation still considers all judged
events.

The correction does not change replay serialization, stored score columns,
the outbox schema, or credential handling.

## Bad points

The Bokutachi Direct Manual `optional.bp` field is calculated as:

```text
BAD + POOR + KPOOR
```

The sum is evaluated in a wide integer and rejected if it exceeds the payload
integer range. The `judgements` object remains unchanged and continues to omit
KPOOR because Bokutachi's BMS judgement shape has no separate KPOOR field.

## Verification

Regression tests cover per-gauge recording, bounded capacity, realtime worker
transfer, result-series order and cycling, state-derived IR history, legacy
replay fallback, and unchanged non-GAS behavior. Payload tests confirm that BP
includes KPOOR and that integer overflow is rejected. Existing replay,
PGREAT-timing, payload-size, and credential-hygiene tests remain green.
