# iOS input latency: PR #135 versus PR #136

The current iOS simulator build delivered touch input to the gameplay worker
faster in both paired runs. Touch-to-worker p95 fell from **8.6 to 4.4 ms**
and **8.2 to 6.5 ms** (49% and 21%). This supports an improvement in software
input delivery for this workload. It does not establish physical touchscreen
to speaker latency, and the sound-command results did not improve consistently.

## Comparison

Each cell is p50 / p95 / p99 / maximum, in milliseconds. Percentiles from the
standard telemetry are upper bounds of 0.1 ms histogram buckets.

| Stage | PR #135 run 1 | Current run 1 | PR #135 run 2 | Current run 2 |
| --- | ---: | ---: | ---: | ---: |
| Touch timestamp → worker | 2.2 / 8.6 / 17.5 / 476.007 | 1.7 / 4.4 / 8.0 / 19.517 | 2.6 / 8.2 / 11.4 / 15.161 | 2.0 / 6.5 / 11.6 / 39.556 |
| Touch timestamp → sound command | 2.2 / 8.4 / 12.8 / 358.028 | 1.9 / 4.4 / 6.8 / 11.462 | 2.5 / 6.3 / 10.1 / 11.580 | 2.7 / 6.7 / 11.7 / 29.874 |
| Sound command → audio callback | 5.2 / 10.3 / 10.6 / 10.715 | 4.9 / 10.3 / 10.6 / 10.694 | 5.4 / 10.3 / 10.6 / 10.667 | 5.0 / 10.1 / 10.6 / 10.706 |

All four runs delivered **600 DOWN/UP edges and 300 sound commands**. The
handler probes paired all 600 edges in each run, with zero missing pairs and
zero synchronously emitted SDL finger events. Both versions already use the
direct UIKit gameplay recognizer introduced before PR #135's final commit.

The more detailed observer shows that the largest repeatable difference was
before UIKit handler entry:

| Detailed p95 stage | PR #135 run 1 / run 2 | Current run 1 / run 2 |
| --- | ---: | ---: |
| Touch timestamp → UIKit handler | 7.528 / 7.501 ms | 2.422 / 3.669 ms |
| UIKit handler → worker enqueue entry | 0.045 / 0.048 ms | 0.037 / 0.041 ms |
| Touch timestamp → worker enqueue entry | 7.551 / 7.532 ms | 2.447 / 3.694 ms |

This is consistent with keeping UIKit's main thread available while scene
updates and rendering run on the application worker. It is an interpretation
of the measured boundaries, not a decomposition of every OS or GPU stall.

Audio-callback delay was essentially unchanged. The second current run's
sound-command p95 was slightly worse (6.7 versus 6.3 ms), as were its p99 and
maximum. The first baseline run had a 476 ms touch-to-worker outlier; the
observer places approximately 475.9 ms before UIKit handler entry. Its cause
was not isolated. Neither that outlier nor the improved p95 supports claiming
that all latency tails have been eliminated.

## Reproduction and controls

- Baseline: exact PR #135 tip `a2c95e8adfd3d06e2de5c957349f720ccf598572`,
  including its final timed presentation pacer.
- Current: `6d50f19128b6938c2d5f5ddb2b8150f547069eea` on PR #136.
- Same booted iPhone Duo, iOS 27.1 simulator, Apple M1 Pro host, arm64 Debug
  configuration, Metal rendering, default skin, fixture, and profile settings.
- Saved `video.frameCap = 0` and VSync enabled for all four runs. All final
  cumulative frame-submission averages were 60.0 FPS. These are application
  submissions, not physical scanout measurements.
- Same generated seven-lane BMS and temporary observer hooks as the previous
  [iOS pacing measurement](2026-10-10-ios-presentation-pacing.md).
- Each fresh app process received 300 sequential Device Hub clicks at
  `(1240, 980)`, after gameplay had started. Input loop durations were 58.225,
  58.931, 59.544, and 60.175 seconds, in order **current 1, baseline 1,
  baseline 2, current 2**. No compiler or test suite ran during the input loops.
- Baseline and current source differences were swapped sequentially in the
  existing checkout to build retained test apps; the branch and submodules
  were unchanged. Build identity text alone is not a baseline identifier:
  the checkout stayed on the current branch. The manifest records the source
  revisions and SHA-256 of each measured executable.
- The simulator negotiated approximately 85–86 frames per audio callback at
  8 kHz in every run. That host audio configuration does not represent the
  physical iOS device's requested 128-frame buffer or speaker latency.

The existing standard histograms capture actual worker consumption and sound
command publication. The detailed hooks observe UIKit handler entry, gameplay
callback entry, and worker enqueue entry, using the same logic on both builds.
Detailed percentiles have 1 µs buckets; values at or above 16.384 ms share an
overflow bucket. None of the detailed p95 values quoted above overflowed.
Observer overhead is present in both versions and was not subtracted.

This is two runs per build, not a randomized statistical trial. Simultaneous
fingers, heavy skins, forced render stalls, physical digitizer delivery,
ProMotion devices, and audible output were not measured. The known physical
iPad was unavailable; the connected Android phone cannot answer the iOS question.

## Restoration and evidence

Temporary instrumentation was removed from the source tree. The normal current
simulator app was rebuilt and restored, the generated chart was removed, and
the original simulator databases and settings were restored. Each test was
terminated before the chart finished. No build was distributed.

- [Comparison data](evidence/2026-10-11-ios-input-latency/comparison.json)
- [Build hashes and workload manifest](evidence/2026-10-11-ios-input-latency/manifest.json)
- [Baseline run 1](evidence/2026-10-11-ios-input-latency/baseline1-snapshot.log)
  and [run 2](evidence/2026-10-11-ios-input-latency/baseline2-snapshot.log)
- [Current run 1](evidence/2026-10-11-ios-input-latency/current1-snapshot.log)
  and [run 2](evidence/2026-10-11-ios-input-latency/current2-snapshot.log)
- [Baseline hook patch](evidence/2026-10-11-ios-input-latency/baseline-instrumentation.patch)
  and [current hook patch](evidence/2026-10-11-ios-input-latency/current-instrumentation.patch),
  with the shared observer sources and chart fixture in the same directory.

The hook patches use zero context (`git apply --unidiff-zero`); copy the shared
`IOSTouchProbe.h` and `.mm` into `src/perf` before building a measurement app.
