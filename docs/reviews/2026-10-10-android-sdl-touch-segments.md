# Android touch → SDL path measurements

Measured on 2026-10-10 after the [whole-path comparison](2026-10-10-android-touch-latency.md). That earlier event-timestamp → sound-command measurement included SDL in the baseline, but did not isolate it. These additional runs measure the boundaries separately.

**SDL was not a large delay before gameplay delivery in this workload.** Java touch entry → the baseline gameplay SDL event watch took **39 µs median / 113 µs p95**. The updated direct JNI path took **39 / 107 µs**. The larger difference was the time until the entire Java touch handler returned: **312 / 1817 µs before, versus 55 / 217 µs after**. That full-handler measurement includes work after gameplay enqueue; it must not be added to current-event gameplay latency. Keeping the input thread occupied can delay subsequent events, but these runs do not quantify that effect under a saturated stream.

## Results

The same physical Samsung Galaxy S20 FE, Android 13, 120 Hz display setting, chart, skin, input coordinates and injector were used. Each build received 300 single-finger gestures followed by 100 three-finger gestures: 600 presses, **1200 press/release edges**, with 40 ms hold and 110 ms rest. All eight counters recorded exactly 1200 samples, with **zero missing boundary metadata**. Menu touches are excluded because counters are committed only when a touch reaches worker enqueue.

Values are **microseconds**, shown as **p50 / p95 / p99 / maximum**. “Route” means the gameplay `SDLTouchInputSource::EventHandler` boundary in the baseline, and `nativeOnRawTouch` JNI entry in the updated build.

| Measured interval | Pre-branch `beb3cd192` | Updated `32683fe18` |
|---|---:|---:|
| Android event timestamp → Java `onTouch` entry | 2336 / 3594 / 4473 / 5624 | 2303 / 3548 / 4444 / 5628 |
| Android event timestamp → route | 2388 / 3700 / 4901 / 6075 | 2355 / 3625 / 4542 / 5670 |
| Java entry → diagnostic JNI entry | 8 / 12 / 34 / 1424 | 7 / 11 / 81 / 701 |
| Java entry → route | 39 / 113 / 737 / 3601 | 39 / 107 / 763 / 1505 |
| Route → gameplay session callback | 3 / 5 / 10 / 2046 | 2 / 5 / 7 / 541 |
| Gameplay session callback → worker enqueue | 9 / 15 / 42 / 1724 | 5 / 13 / 25 / 2330 |
| Java entry → worker enqueue, paired | 52 / 198 / 872 / 3617 | 48 / 179 / 787 / 2374 |
| Entire Java touch handler | 312 / 1817 / 3072 / 4525 | 55 / 217 / 799 / 2422 |

The Android timestamp → SDL-watch result is therefore **2.388 ms median / 3.700 ms p95**, including Android dispatch before Java receives the event. The direct counterpart is **2.355 / 3.625 ms**. Both are paired endpoint measurements, not sums of percentile rows.

The full-handler median decreased by approximately **82%**, and p95 by **88%** in these runs. Java entry → worker enqueue changed only from 52 to 48 µs median and 198 to 179 µs p95. This does not support a claim that bypassing SDL removed several milliseconds from ordinary current-event delivery. It does show a shorter full handler and fewer dependencies on SDL during gameplay. Occasional outliers remain on both builds: the updated gameplay-callback → enqueue maximum was larger, so improvement is not uniform across all stages or maxima.

Single-finger-only results are preserved separately. Before/after Java entry → route was **45/252 µs versus 40/91 µs** at p50/p95; full handler was **477/2157 versus 57/261 µs**. The combined distribution includes these singles and the chord samples; no chord percentile is inferred by subtraction.

## What the SDL segment includes

The pre-branch path is:

```text
MotionEvent timestamp
  → Android delivery → AsoBMaShowSurface.onTouch entry
  → unmodified SDLSurface.onTouch
  → SDLActivity.onNativeTouch
  → Android_ActivityMutex → Android_OnTouch
  → SDL_SendTouch: touch lock, finger bookkeeping, SDL_PushEvent
  → SDL event-watch lock/filter/watch dispatch
  → SDLTouchInputSource::EventHandler
  → gameplay session callback → touch-router lock/routing → worker enqueue
```

The SDL event watch delivers gameplay input synchronously on the producer thread. `SDL_PushEvent` calls event watches **before** adding the event to the ordinary event queue. The gameplay callback does not wait for the main loop to poll the queue. Java entry → route includes the elapsed time spent acquiring SDL locks and running preceding processing, but individual lock waits were not instrumented.

The entire-handler measurement additionally covers the remainder of SDL dispatch/queue insertion and `SDLSurface`'s `ScaleGestureDetector.onTouchEvent`, up to the wrapper's `finally` entry. It also includes the diagnostic work itself. These measurements do **not** isolate how much of the longer baseline return path belongs to SDL internals versus Java gesture detection; attributing all of it to an SDL mutex would be unsupported.

The updated route is the gameplay-owned direct JNI path. Its route → gameplay interval includes timestamp handling and native registration dispatch. The gameplay-callback → enqueue interval starts before taking the app's touch-router mutex on both builds.

## Instrumentation and limits

Both builds use the same measurement-only header, Java entry clock and JNI begin/end hooks. It tracks boundaries in thread-local storage and records fixed atomic histograms with 1 µs buckets, with no per-event logging. This replaces the earlier baseline's source-association table for these segmented runs; no SDL timestamps, routing, audio settings or gameplay behavior are changed. The SDL submodule is untouched. These diagnostic patches were removed from application sources after measurement.

The wrapper adds one JNI call before dispatch and one after. Java entry → diagnostic JNI entry is reported to expose part of that overhead; it is not subtracted from any percentile. Full-handler timing includes the probe updates performed inside dispatch and excludes the final diagnostic JNI call by capturing its endpoint in Java first. Histograms are cumulative approximate snapshots, taken after injection stopped. Android 13 source timestamps have **1 ms resolution**, despite the finer in-app counters. The initial touches and their warm-up costs are included.

Input is injected on the phone and traverses Android dispatch. Physical digitizer scanning, speaker/DAC output and touch-to-photon latency remain outside scope. No deliberately stalled SDL callback or saturated continuous-motion workload was introduced. One sequential run per build is insufficient to distinguish small timing differences from scheduling and thermal variation. Order was updated → baseline; battery temperatures were 32.8→33.2 °C and 32.9→33.3 °C respectively, at 100% charge. App average FPS remained approximately 118. The stronger audio-callback improvement measured previously is separate from these SDL results.

## Evidence and restoration

- [Baseline combined](evidence/2026-10-10-android-touch-latency/segments-baseline-combined.txt), [updated combined](evidence/2026-10-10-android-touch-latency/segments-updated-combined.txt), [baseline singles](evidence/2026-10-10-android-touch-latency/segments-baseline-single.txt), [updated singles](evidence/2026-10-10-android-touch-latency/segments-updated-single.txt), and [JSON comparison](evidence/2026-10-10-android-touch-latency/segments-comparison.json). Injector completion logs are alongside these files.
- [Baseline instrumentation patch](evidence/2026-10-10-android-touch-latency/segments-baseline-instrumentation.patch) applies to `beb3cd192`; [updated instrumentation patch](evidence/2026-10-10-android-touch-latency/segments-updated-instrumentation.patch) applies to `32683fe18`. Both patch applications were checked against separate temporary Git indexes. Both arm64 native builds and signed release packaging passed. A host check exercised probe lifecycle and missing-boundary detection.
- Baseline diagnostic APK SHA-256: `eaae44a0a6c0227fcfa026a26bac183f94b9caecb0e0b13ea672d37a79a0af94`. Updated diagnostic APK SHA-256: `4cef0270710cbdf31faf21adbfe48c30992e151ab5ae22b7d5e86194347b1e46`.
- The branch and production sources were restored. The local Android telemetry option was returned to OFF. The phone was reinstalled in place with the previously verified updated version-6 APK (`834de9bd8226b65a6e4ad5ac27fb4cc6d24a97fde0c1791de44d0aa870af664e`), preserving user data. The temporary chart and injector were removed. No distribution upload occurred.
