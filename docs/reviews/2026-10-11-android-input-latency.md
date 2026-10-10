# Android input latency after event/render separation

**These measurements do not show a material reduction in ordinary gameplay
input latency beyond the build measured in PR #135.** Event timestamp to sound
command had the same 3.0–3.1 ms median range. Its p95 was 5.3–5.4 ms for the
PR #135 measurement build and 5.4–5.6 ms for the current branch. Audio-command
waiting remained about 2 ms at p95. The current build's tails were not lower;
these four short runs do not establish whether the small differences represent
a regression rather than scheduling variation.

## Method

Repeated the [PR #135 whole-path protocol](2026-10-10-android-touch-latency.md)
on the same Samsung Galaxy S20 FE, Android 13, Vulkan, 2400×1080 landscape and
120 Hz display mode. Used the exact retained chart, keysound, injector and
baseline APK from that comparison, verifying its published SHA-256
`834de9bd8226b65a6e4ad5ac27fb4cc6d24a97fde0c1791de44d0aa870af664e`.
This baseline is the APK actually measured in PR #135, not a new build of its
final Git head. The current APK was built from the clean `cd04112c6` checkout
with the existing `ASOBMASHOW_ENABLE_PERF_TELEMETRY=ON` option. The histogram and
realtime-worker source files are unchanged from PR #135.

Run order was baseline 1 → current 1 → current 2 → baseline 2, each in a fresh
process with roughly 24 seconds of gameplay warm-up before injection. Each run
used 300 single-finger gestures at `(730,840)`, then 100 three-finger gestures
at x=`730,1130,1530`, y=`840`, with 40 ms holds and 110 ms rests. The Java
injector ran on the phone, assigning source timestamps immediately before
Android input injection; individual USB round trips were outside the interval.

Every run recorded **600 successful sound commits and 1,200 worker edges**:
2,400 presses and 4,800 edges across the four runs. JNI ingress recorded 900
samples for singles and 2,400 cumulatively, including intermediate pointer
samples. All runs used the speaker at volume 7/15, exclusive AAudio, 48 kHz and
96 frames per app callback. Average app FPS was 118.1–118.2. Battery was 100%
on USB; temperature ranges were 35.1→35.1, 33.5→34.0, 34.0→34.2 and 34.2→34.5 °C
in run order. The phone cooled while the measurement APK compiled.

## Results

Values are **milliseconds: p50 / p95 / p99 / maximum**. Percentiles are the
existing histogram's 100 µs bucket upper bounds. Combined values include the
single-finger subset; no chord percentile is inferred by subtraction.

| Interval, combined workload | Baseline 1 | Current 1 | Current 2 | Baseline 2 |
| --- | --- | --- | --- | --- |
| Android event → JNI ingress | 2.3 / 3.6 / 4.3 / 5.521 | 2.3 / 3.4 / 4.5 / 5.520 | 2.2 / 3.4 / 4.3 / 5.280 | 2.3 / 3.4 / 4.2 / 5.581 |
| Worker enqueue → observation | 0.3 / 1.7 / 2.5 / 4.051 | 0.3 / 1.7 / 2.5 / 4.205 | 0.4 / 2.1 / 3.2 / 4.830 | 0.3 / 1.8 / 2.7 / 3.488 |
| Android event → worker observation | 2.8 / 4.9 / 6.2 / 7.570 | 2.7 / 4.9 / 6.1 / 6.913 | 2.8 / 5.1 / 6.6 / 8.371 | 2.8 / 4.9 / 6.1 / 7.148 |
| Android event → sound command | 3.1 / 5.3 / 6.5 / 7.604 | 3.0 / 5.4 / 6.4 / 6.943 | 3.1 / 5.6 / 6.7 / 8.397 | 3.0 / 5.4 / 6.6 / 7.184 |
| Sound command → audio callback | 1.0 / 1.9 / 2.0 / 2.049 | 1.1 / 2.0 / 2.1 / 2.233 | 1.1 / 2.0 / 2.0 / 2.009 | 1.0 / 1.9 / 2.0 / 2.125 |

| Single-finger subset | Baseline 1 | Current 1 | Current 2 | Baseline 2 |
| --- | --- | --- | --- | --- |
| Android event → sound command | 3.4 / 5.6 / 6.5 / 7.604 | 3.3 / 5.7 / 6.6 / 6.943 | 3.4 / 5.8 / 6.9 / 8.397 | 3.2 / 5.7 / 6.6 / 6.911 |
| Sound command → audio callback | 1.0 / 1.9 / 2.0 / 2.027 | 1.1 / 2.0 / 2.1 / 2.233 | 1.0 / 2.0 / 2.0 / 2.009 | 1.0 / 1.9 / 2.0 / 2.125 |

The second current run had a longer worker-enqueue tail, while its Android-to-JNI
tail was slightly shorter. The paired event-to-command result therefore matters
more than selecting an improved individual segment. Maximum event-to-command
latency also varied in both directions between builds.

PR #135 already delivered Android gameplay touches directly through Java/JNI
into an independent gameplay worker. Separating the SDL event pump from the
application/render owner does not replace that direct route. The observed
ordinary-play timings are consistent with retaining the earlier latency gains;
the separate [stall and lifecycle checks](2026-10-11-desktop-android-device-acceptance.md)
address event-pump independence. This experiment did not inject a render stall.

## Limits, evidence and restoration

Android source timestamps have millisecond resolution. These are software
intervals, not physical finger-to-speaker or touch-to-photon measurements. Do
not add percentile columns to estimate end-to-end output latency. Output
presentation timestamps and reliable underrun counters remain unavailable.
Two repetitions per build, a synthetic chart, varying temperature and ordinary
OS scheduling do not support sub-millisecond causal claims or a device-wide
performance guarantee.

[Evidence](evidence/2026-10-11-android-input-latency/) contains all eight
single/combined snapshots, injector completion records, machine-readable
comparison, controls, APK hashes and the exact fixture. The injector source and
commands are linked in the earlier report. The current measurement APK's
separately compiled identity object retained its prior marker; `builds.json`
records that marker alongside the actual clean build checkout and APK checksum.
The restoration build refreshed the marker to `cd04112c6`, clean.

Both measurement and normal release builds passed. Restored the normal,
telemetry-OFF version-code-6 APK in place, restored the original media volume
6/15, and removed the owned chart and injector. The library returned to its
original nine charts; existing app data was preserved. No production source
changes, database edits, or distribution upload were made for this measurement.
