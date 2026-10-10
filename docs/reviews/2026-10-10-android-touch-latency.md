# Android touch latency and surface lifecycle verification

Measured 2026-10-10 on a physical Samsung Galaxy S20 FE (SM-G781N, SM8250, Android 13, arm64). This verifies the changes following `c323ee2d` on `fix/android-input-latency`. The signed release APK was installed in place as version code **6**, preserving app data. APK SHA-256: `834de9bd8226b65a6e4ad5ac27fb4cc6d24a97fde0c1791de44d0aa870af664e`.

A subsequent [segmented Android → SDL measurement](2026-10-10-android-sdl-touch-segments.md) isolates Java delivery, the SDL gameplay event watch, worker enqueue and full touch-handler duration. It found little difference before gameplay delivery under this workload, but substantially shorter full-handler duration on the updated path.

## Pre-branch comparison

The baseline is **`beb3cd192`**, the commit from which `fix/android-input-latency` was created (confirmed by the branch reflog), rather than the preceding fix commit. Its input routing, worker scheduling, SDL configuration, miniaudio implementation and audio settings were retained. Only timing instrumentation was added. The baseline was built and installed as version code 6, then the updated APK was reinstalled and measured again. App data was preserved throughout.

Each run used the same 300 single-finger gestures followed by 100 three-finger gestures: **600 presses / 1200 press-release worker observations per run**. Order was updated run 1 → pre-branch → updated run 2. Each used a fresh app process and the same phone, chart, skin, coordinates, speaker, volume, 120 Hz setting and injection method. All 600 presses reached a successful sound commit in all three runs. Percentiles below are histogram upper bounds in milliseconds.

| Metric | Pre-branch | Updated run 1 | Updated run 2 |
|---|---:|---:|---:|
| Event timestamp → sound command, p50 | 3.4 | 3.1 | 3.1 |
| Event timestamp → sound command, p95 | 5.9 | 5.7 | 5.4 |
| Event timestamp → sound command, p99 | 6.8 | 6.5 | 6.4 |
| Event timestamp → sound command, max | 9.323 | 10.783 | 8.517 |
| Sound command → audio callback, p50 | 5.1 | 1.0 | 1.1 |
| Sound command → audio callback, p95 | 9.7 | 2.0 | 1.9 |
| Sound command → audio callback, p99 | 10.0 | 2.0 | 2.0 |
| Sound command → audio callback, max | 10.016 | 2.055 | 2.713 |
| App audio callback frames at 48 kHz | 480 (10 ms) | 96 (2 ms) | 96 (2 ms) |
| AAudio sharing mode | Shared | Exclusive | Exclusive |

The strongest observed improvement is **about 78–80% less median waiting from a sound command to the audio callback**, and about **79–80% less at p95**. Event-to-command median improved by approximately **9%** (0.3 ms); its p95 improved by approximately 3–8% (0.2–0.5 ms). Those smaller input differences should not be overgeneralized from one device and workload. Maximum input-to-command latency did not improve consistently: updated run 1 contained a larger outlier than the baseline. These runs do not quantify behavior under deliberately stalled SDL callbacks; the direct-path regression tests cover that condition separately. Do not add percentile columns to invent a physical end-to-end result.

For the single-finger subset alone, event-to-command p50/p95/p99 was **3.9/6.1/7.3 ms** before the branch, **3.2/6.1/6.7 ms** in updated run 1, and **3.5/5.6/6.4 ms** in updated run 2. Chord and single-touch samples are therefore reported both separately and combined; no chord-only percentile is inferred by subtracting cumulative percentiles.

The baseline observer records the original `MotionEvent.getEventTime()` before calling the unmodified `SDLSurface.onTouch`. An SDL event watch associates that timestamp with the event identity, and the existing gameplay touch callback copies it into a telemetry-only worker field. It leaves the SDL timestamp used by judgement unchanged. Every baseline gameplay edge matched its original timestamp: **1200 matched, 0 missing**. This avoids the misleading comparison of current Android source time against a later baseline SDL timestamp. The observer adds some baseline instrumentation overhead, so the small input gain is less conclusive than the large audio gain. The source timestamp to sound-command endpoints and histogram implementation are identical across builds.

The [instrumentation patch](evidence/2026-10-10-android-touch-latency/prebranch-instrumentation.patch) applies cleanly to the baseline commit. A host check verified that source metadata survives an event copy between threads without changing the SDL timestamp. The instrumented baseline native build and signed release packaging passed; no baseline performance fixes were backported. Baseline APK SHA-256: `00d9b3b0f1315d8c24be0d488cadfc11406a36449bc1410a3323fc88b4d88db2`.

App average FPS remained approximately 118 in all runs. Battery stayed at 100% on USB. Battery temperature was 30.8 °C before the baseline and 31.8 °C after it; updated run 2 ended around 32.2 °C. This was a short sequential comparison, without randomization or a controlled thermal chamber. Neither build provides output presentation timestamps or reliable underrun counts through this telemetry, and physical digitizer/speaker delay remains outside the measurement.

Comparison evidence: [baseline single-touch](evidence/2026-10-10-android-touch-latency/prebranch-single-touch.txt), [baseline combined](evidence/2026-10-10-android-touch-latency/prebranch-combined-touch.txt), [updated repeat single-touch](evidence/2026-10-10-android-touch-latency/updated-repeat-single-touch.txt), [updated repeat combined](evidence/2026-10-10-android-touch-latency/updated-repeat-combined-touch.txt), and [machine-readable summaries](evidence/2026-10-10-android-touch-latency/comparison.json). Injector completion logs are alongside those snapshots.

The repository was returned to `fix/android-input-latency`; temporary baseline instrumentation was removed from application sources. The phone was restored to the updated instrumented version-6 APK, and the synthetic chart/injector were removed after the final run.

## Initial updated-build run

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

The Android audio backend does not expose output render timestamps or reliable underflow counts through this telemetry. Therefore `underflow reporting unknown (0)` is **not evidence of zero underruns**, and a 2 ms callback block is not total output latency. Physical touch scanning, DAC/speaker delay, and touch-to-photon delay remain unmeasured. The pre-branch comparison below uses separate instrumented process runs on the same phone; it does not measure physical touch-to-speaker latency.

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
