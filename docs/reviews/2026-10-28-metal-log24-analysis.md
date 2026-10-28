# Capture 24: younger frames, less protection against CPU spikes

> The later-preparation recommendation below is withdrawn. The deliberate wait
> was removed in `921a82d5`: a lower preparation-age metric did not establish
> better input responsiveness, and capture 24 exposed extra deadline misses.
> Keep the measurements as historical evidence, not a pacing recommendation.

**Decision after review with the user:** Remove deliberate pre-input waiting entirely. The 1 ms proposal below is superseded. A smaller frame-age metric is not sufficient evidence of better rhythm-game responsiveness. Keep immediate input/scene processing and the diagnostic capture.

The later-preparation experiment is active and produces the intended median
improvement: preparation-to-display age falls from **16.615 ms to 12.789 ms**,
a **3.825 ms (23.0%)** reduction against capture 23. The median callback-to-
preparation gap rises from 0.007 ms to 3.845 ms, while preparation-to-request
CPU time remains essentially unchanged (1.140 versus 1.141 ms).

This is a useful result, but not an unqualified cadence win. Two CPU spikes
consume the reduced deadline margin and coincide with missing display records
and two-refresh display gaps. The current 4 ms delay cap needs a more conservative
comparison before becoming a settled policy.

## Source and method

Input is `AsoBMaShow-performance-log 24.txt`, compared with log 23 using the same
schema and cohort rule: all gameplay-tagged records with a nonzero display time,
without statistical trimming. Calculate differences within each frame, then
aggregate; quantiles interpolate at `(n - 1) * p`. Submission-miss counts separately
include submitted frames without display times. The captures are separate runs,
not matched frame pairs; chart/build/device/thermal identity is not established
by this log format.

Capture 24 contains 8,192 consecutive IDs, 169–8360, over 69.484 seconds. It has
one lifecycle generation and zero recorded contention losses. The ring has
overwritten the first 168 callbacks, but retains the gameplay-tagged segment
1297–8156. Every retained callback has a preparation-start timestamp. Numeric
fields are finite and displayed gameplay GPU/display times are ordered.

[Derived comparison](data/2026-10-28-metal-log24-comparison.json) preserves source
hashes, cohort sizes, distribution summaries and exception IDs. Raw logs with
private library paths are not committed.

## Comparison

| Metric | Capture 23 | Capture 24 |
| --- | ---: | ---: |
| Gameplay submissions | 6,761 | 6,769 |
| Gameplay display timestamps | 6,754 | 6,765 |
| Submitted gameplay frames without display times | 7 | 4 |
| Median preparation → display | 16.615 ms | 12.789 ms |
| P95 preparation → display | 16.656 ms | 15.033 ms |
| P99 preparation → display | 16.665 ms | 16.530 ms |
| Median GPU work | 1.122 ms | 1.109 ms |
| Median GPU end → display | 14.139 ms | 10.137 ms |
| Median remaining CPU-request deadline margin | 7.140 ms | 3.354 ms |
| Late gameplay CPU requests | 1 (0.0148%) | 3 (0.0443%) |
| Gameplay display times over 1 ms after target | 0 | 2 |

The mean preparation-to-display age also improves, from 16.604 to 13.010 ms.
The upper tail improves less than the median, consistent with conservative
warm-up and post-spike behavior. Median display-minus-target remains close to
zero (+0.008 versus +0.011 ms). This measures age since preparation, not physical
input-to-photon latency or proof of improved judgement timing.

## The three late gameplay submissions

| Frame | Added wait | CPU preparation → request | Request past deadline | Display outcome |
| --- | ---: | ---: | ---: | --- |
| 1378 | 2.681 ms | 18.417 ms | 12.891 ms | Display 16.669 ms after target |
| 2036 | 3.866 ms | 6.927 ms | 2.487 ms | No display timestamp; surrounding displays 16.667 ms apart |
| 7798 | 3.421 ms | 5.224 ms | 0.346 ms | No display timestamp; surrounding displays 16.667 ms apart |

Frame 1378's CPU work alone exceeds the available callback-to-deadline interval;
it would remain late without the deliberate wait under unchanged-work assumptions.
The following frame 1379 also displays 3.734 ms after target, despite an early
request. These are the two late displayed records.

For frames 2036 and 7798, subtracting the measured wait from request time would
put each before its deadline, assuming the work/scheduling otherwise stayed the
same. Thus the wait demonstrably consumes the headroom those spikes needed.
The record does not prove identical execution timing in a no-wait counterfactual,
nor does a missing `presentedTime` alone prove a specific dropped-frame mechanism.
The corresponding two-refresh gaps strengthen the practical concern.

Capture 24 has 6,759 near-one-refresh gameplay display intervals, three
16.667 ms gaps, one 25.001 ms gap, and one 766.678 ms gap. The third two-refresh
gap is near startup (1298→1299); frame 1298 displayed about one refresh before
its own target. The large final gap is mostly no-submission/idle callbacks while
the gameplay flag remains set. It must not be treated as a steady-rendering stall.
Capture 23 also has startup/stall and final idle gaps, including a 125 ms CPU-stall
gap. These short unmatched captures do not establish a statistically reliable
change in rare-stall frequency.

## Recommended next comparison

Back off the maximum deliberate delay to **1 ms** for the next conservative run,
keeping the workload estimator, safety margin and capture unchanged. Under the
same-work assumption, that cap leaves both observed moderate spikes' requests
and scheduled-handler observations before their deadlines, while the 18.4 ms
stall still requires separate investigation. This is a sensitivity observation,
not a guarantee for unseen spikes or scheduler oversleep.

The smaller cap deliberately gives up some of the 3.8 ms median improvement to
restore deadline headroom. If a larger gain remains the goal, identify and reduce
the CPU preparation spikes before spending that margin again. Do not increase
the delay based only on the better median.

No runtime change or deployment accompanies this analysis.
