# Bokutachi Adopted Gauge and BP Design

## Goal

Correct Bokutachi Direct Manual payloads so gauge history represents only the
gauge adopted at the end of Gauge Auto Shift (GAS), and BP includes KPOOR.

## Gauge history

`makeIrSubmission` remains the boundary that derives provider-neutral evidence
from the persisted replay. A gauge-mutating replay event is a judged press,
release, or miss, a mine event, or an explicit gauge event.

The adopted gauge is the `gaugeType` on the final gauge-mutating replay event.
Every gauge-mutating event is still validated for a finite gauge value and is
still considered when deriving PGREAT early/late counts. Only events whose
`gaugeType` equals the adopted gauge contribute samples to
`IrSubmission::gaugeHistory`. This removes values from gauges used during GAS
transitions. A replay with no gauge-mutating events continues to produce no
gauge history, and a non-GAS replay retains all of its samples because every
event uses the same gauge type.

This correction does not change replay storage, result-screen graph behavior,
or the outbox schema.

## Bad points

The Bokutachi Direct Manual `optional.bp` field is calculated as:

```text
BAD + POOR + KPOOR
```

The sum is evaluated in a wide integer and rejected if it exceeds the payload
integer range. The `judgements` object remains unchanged and continues to omit
KPOOR because Bokutachi's BMS judgement shape has no separate KPOOR field.

## Verification

Regression tests cover a replay that changes gauge type and confirm that only
samples from the final adopted gauge survive. Payload tests confirm that BP
includes KPOOR and that integer overflow is rejected. Existing no-GAS gauge
history, PGREAT timing, payload-size, and credential-hygiene tests remain
green.
