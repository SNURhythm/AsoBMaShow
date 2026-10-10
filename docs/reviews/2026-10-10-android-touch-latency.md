# Android touch latency and surface lifecycle verification

Measured 2026-10-10 on a physical Samsung Galaxy S20 FE (SM-G781N, SM8250, Android 13, arm64). This verifies the changes following `c323ee2d` on `fix/android-input-latency`. The signed release APK was installed in place as version code **6**, preserving app data. APK SHA-256: `834de9bd8226b65a6e4ad5ac27fb4cc6d24a97fde0c1791de44d0aa870af664e`.

## Result

For 600 injected gameplay presses, Android event timestamp to successful sound command commit measured **≤3.1 ms median, ≤5.7 ms p95, ≤6.5 ms p99; maximum 10.783 ms**. This is a paired measurement on each committed press, including event delivery and worker scheduling. It is not a physical finger-to-speaker measurement or an isolated JNI-call benchmark.

| Stage | Samples | p50 ≤ ms | p95 ≤ ms | p99 ≤ ms | Max ms |
|---|---:|---:|---:|---:|---:|
| Android event timestamp → JNI entry | 2400 | 2.3 | 3.6 | 4.7 | 5.618 |
| Worker enqueue → worker observation | 1200 | 0.3 | 1.7 | 2.8 | 3.819 |
| Android event timestamp → worker observation | 1200 | 2.8 | 5.2 | 6.1 | 7.485 |
| Android event timestamp → sound command commit | 600 | 3.1 | 5.7 | 6.5 | 10.783 |
| Sound command enqueue → audio callback drain | 600 | 1.0 | 2.0 | 2.0 | 2.055 |

The first 300 single-finger presses, before adding chords, measured event-to-command **≤3.2 / 6.1 / 6.7 ms** at p50/p95/p99, maximum **10.783 ms**. Combined statistics include those 300 presses and another 300 presses from 100 three-finger gestures; they are not a separate chord-only distribution.

Histograms are cumulative approximate snapshots with 100 µs buckets; percentile values are bucket upper bounds. The Android 13 event source used millisecond timestamps. Stages have different populations: JNI includes intermediate pointer samples, worker observations include press/release edges, and sound commits count successful sound-producing presses. Do not add these percentiles to infer an end-to-end percentile.

## Device and workload

- Display configured at 120 Hz. App telemetry reported about 118.2 average FPS during measurement; this does not measure physical display presentation.
- Vulkan renderer; AAudio exclusive output; 48 kHz stereo; 96 frames per app callback (2 ms), with a reported hardware burst of 48 frames.
- Built-in speaker, media volume 7/15. USB connected, battery 100%; battery temperature 27.6 °C before preparation and 30.0 °C after measurement/lifecycle checks.
- Release optimizations, `ASOBMASHOW_ENABLE_PERF_TELEMETRY=ON`, ordinary non-debuggable release package. No debugger attached. Telemetry records fixed atomic histograms and logs summaries outside realtime paths.
- Synthetic 120 BPM seven-key chart, 256 bars, a short generated click keysound, built-in perspective gameplay skin, non-autoplay native input authority (epoch 1).
- 300 single-finger gestures at `(730, 840)` on the 2400×1080 screen, followed by 100 three-finger gestures at x=`730,1130,1530`, y=`840`. Each gesture held 40 ms and rested 110 ms; actual runs lasted 47.534 s and 16.616 s including dispatch overhead.
- Input was injected by a Java process **on the phone**, launched once through ADB. The source timestamps were assigned on-device immediately before `InputManager.injectInputEvent`; USB command round trips are outside the measured interval. Contacts use `TOOL_TYPE_FINGER` and touchscreen source, and traverse Android event dispatch and the app's Java/JNI path. This bypasses the physical digitizer.
- All 600 presses produced measured sound command commits. Screenshot judgement totals showed ongoing gameplay responses. The deliberately unsynchronized taps are not a judgement-accuracy benchmark.

The audio callback duration p50/p95/p99 was ≤0.1 ms; maximum 0.464 ms. Callback intervals were ≤2.0 / 2.1 / 2.2 ms, maximum 3.580 ms. These audio histograms include the menu/preparation interval. Frame-submit maximum includes startup and is not a steady-state frame-latency claim.

The Android audio backend does not expose output render timestamps or reliable underflow counts through this telemetry. Therefore `underflow reporting unknown (0)` is **not evidence of zero underruns**, and a 2 ms callback block is not total output latency. Physical touch scanning, DAC/speaker delay, and touch-to-photon delay remain unmeasured. There is no instrumented pre-change baseline, so these results do not establish an A/B speedup.

## Changes verified

Gameplay-owned finger gestures now latch the native session epoch on DOWN and bypass SDL on the Java UI thread for the entire gesture. UI presentation is delivered through a bounded native queue drained on the main thread. A paused/replaced session, queue overflow, or stale gesture cannot fall back into SDL or activate a new session. Normal menu gestures retain SDL delivery. Mixed pen/mouse events retain their separate compatibility path; this measurement covers finger-only gameplay.

The Android Vulkan resume failure was reproduced before the fix, including a render-thread crash through `RefBase::incStrong` → Vulkan Android surface creation after SDL released its native window. The fix retains the native window under SDL's property lock, suspends rendering before Java destroys the surface, and advances two bgfx frames before acknowledging suspension so the pending present retires. On resume, the old window remains owned until bgfx consumes its replacement.

Physical-device checks passed three Home/background → resume cycles during gameplay, with moving notes and six additional responsive taps after each cycle. Pause, Retry (new epoch 2), and Exit also worked. No rendering pause timeout, fatal signal, or Java fatal exception appeared in the measured process log. The previously failing Android 10 Vulkan emulator also passed three title resume cycles and a gameplay resume, pause, Retry, and Exit check after the fix. The surface wait remains bounded at two seconds; a render-thread stall beyond that timeout and external export/activity flows were not stress-tested here.

## Validation and cleanup

- Full desktop build passed, followed by **470/470 CTest tests**, parallel `-j 6`, 109.41 s.
- Android NDK 28.2.13676358 arm64 native build and signed restricted-file release packaging passed. Package command: `scripts/android_firebase_deploy.sh --build-only --version-code 6`.
- Java tests cover API 28/33/34/36 timestamp behavior, gesture ownership and tool splitting; native tests cover stale epochs, queue overflow/cancellation, window retention and render suspension ordering. Paired telemetry tests cover original touch timestamps surviving lane-source normalization.
- No distribution upload. The phone retains the instrumented version-6 build. Repository telemetry defaults and the local Android CMake option were restored to OFF for subsequent normal builds.
- Exited the synthetic chart before completion, removed its temporary directory and the on-device injector, then restarted the app for library reconciliation. Existing charts, settings and app data were retained.

## Evidence and reproduction

Sanitized evidence: [single-touch snapshot](evidence/2026-10-10-android-touch-latency/single-touch.txt), [combined snapshot](evidence/2026-10-10-android-touch-latency/combined-touch.txt), [single injector result](evidence/2026-10-10-android-touch-latency/single-probe.txt), [chord injector result](evidence/2026-10-10-android-touch-latency/chord-probe.txt), [lifecycle log](evidence/2026-10-10-android-touch-latency/lifecycle.txt), and [injector source](evidence/2026-10-10-android-touch-latency/TouchLatencyProbe.java).

For reproduction, compile the injector against an installed Android SDK `android.jar`, dex it with build-tools `d8`, and push that dex into `/data/local/tmp`. With the diagnostic app in a long synthetic chart, run:

```sh
adb shell CLASSPATH=/data/local/tmp/probe.dex app_process /system/bin TouchLatencyProbe 300 40 110 840 730
adb shell CLASSPATH=/data/local/tmp/probe.dex app_process /system/bin TouchLatencyProbe 100 40 110 840 730 1130 1530
```

Coordinates depend on the current skin and screen dimensions. Collect the app PID's logcat, and take a completed histogram snapshot before pausing or backgrounding. Start a new app process for independent distributions; this telemetry does not reset between gameplay sessions. To measure actual finger-to-speaker latency, use an external touch reference and microphone/loopback capture.
