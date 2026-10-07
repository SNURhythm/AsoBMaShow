# Chart loading: exact integer PCM resampling

## Change and cause

Chart loading on the connected Galaxy S20 FE spent most of its time preparing decoded audio. The shared linear PCM resampler evaluated each sample with `long double`. Android arm64 uses software arithmetic for that 128-bit type, making this inexpensive interpolation unusually expensive.

`audio::ResamplePcm` now advances the source position with an exact integer quotient/remainder and uses signed 64-bit weighted interpolation. The phase does not accumulate floating-point error. Output length, channel separation, end clamping, same-rate copying and linear interpolation are preserved. Exact half samples round away from zero consistently across platforms; this corrects occasional one-unit differences from the former floating-point calculation. Products of an int16 sample and a positive int sample rate fit comfortably in int64.

For 44.1 to 48 kHz, each output frame advances by exactly 147/160 of a source frame. The numerator retains the complete interpolation fraction until the final rounding to the existing 16-bit PCM output. This adds no intermediate quantization loss. Integer DSP arithmetic is established practice; for example, [Arm CMSIS-DSP provides both integer and floating-point interpolation](https://arm-software.github.io/CMSIS-DSP/v1.15.0/group__LinearInterpolate.html). Linear interpolation still has its existing filtering limitations, especially for downsampling; improving anti-alias filtering is separate from this arithmetic optimization.

The implementation is shared by Android, iOS, macOS and Windows. No platform API, output sample rate, worker count or decoding dependency was changed.

## Device measurements

Galaxy S20 FE, Android 13, eight reported CPUs, six existing audio workers. The same Fresco Type 38 chart used 1,227 sound resources, 91,285,938 decoded source frames and two visual resources. Each run launched a fresh app process, selected the chart and waited for background loading to finish without pressing Start during loading. Filesystem caches were warm. Measurements are chart resource loading after parsing, not picker-to-gameplay latency or cold-storage results.

| Wall-clock stage | Baseline | Integer round 1 | Integer round 2 |
| --- | ---: | ---: | ---: |
| Resolve sound paths | 1,280 ms | 1,260 ms | 1,233 ms |
| Load sounds | 15,003 ms | 2,686 ms | 2,291 ms |
| Parsed log to Chart loaded log | 16.316 s | 3.984 s | 3.557 s |

Total measured resource loading fell by 76–78%, approximately 4.1–4.6 times faster. Sound loading fell by 82–85%. Path resolution remains approximately 1.3 seconds.

Temporary instrumentation separately accumulated worker elapsed times: decoding was 12.095 / 13.226 / 11.562 seconds and PCM preparation was 72.968 / 1.485 / 1.264 seconds. These sums overlap across workers and must not be read as wall-clock duration. All temporary instrumentation was removed from the final source.

A standalone optimized NDK probe resampling ten seconds of stereo PCM from 44.1 to 48 kHz took 242.675 ms before and 4.685 ms after, about 52 times faster. A deliberately generous 50 ms device budget failed before and passed after; ordinary CTest has no timing gate. The equivalent macOS probe improved from 3.031 to 2.427 ms, about 20% less time. These are single-probe results rather than a broad platform benchmark.

An Android comparison over 147 combinations of seven rates (8–192 kHz), one/two/six channels and deterministic 1,024-frame inputs checked 1,242,963 output samples. Output lengths matched; 9,444 samples differed and the maximum absolute difference was one PCM unit.

## Hardware acceleration and system conversion

The current bundled FFmpeg `libswresample` is configured with `--disable-asm --disable-x86asm` and reports no CPU feature flags. Its default-filter conversion of the same ten-second stereo input, including initialization and flushing, took 27.9–37.4 ms. That filter differs from the existing linear interpolation, so this is not a quality-equivalent comparison. It did not justify replacing the shared converter in this change. FFmpeg has an [AArch64 NEON resampling implementation](https://ffmpeg.org/doxygen/7.1/aarch64_2resample__init_8c_source.html), but enabling it would require separate dependency and output-quality validation.

Android's [low-latency audio guidance](https://developer.android.com/games/sdk/oboe/low-latency-audio) recommends the device's natural output rate and application-side conversion when needed. Changing the output stream rate to rely on system resampling can select a higher-latency path. No hardware offload or platform-specific converter was adopted; the measured shared optimization removes the dominant preparation cost without changing the playback backend.

## Verification

- Literal expected-output regressions cover fractional-rate signed ties, signed extrema, end clamping, downsampling, multiple phase carries, odd denominators and extreme positive rates. The old implementation failed the exact half-sample regression.
- The new resampling tests pass as a standalone Android NDK executable, including the odd-denominator case.
- The desktop `main` target and all five focused CTests passed: resampling, audio mixing, audio-wrapper lifecycle, jukebox restoration and chart audio rendering. The signed restricted-file-access Android release build also passed; the following native build performed only the routine build-identity check, with no compilation or linking.
- The final APK, with profiling removed, loaded the same chart from the Parsed log to Chart loaded in **3.787 seconds**. Gameplay started with the existing custom skin and BGA, AAudio reported successful stream starts, and the app showed no fatal exception or native crash during the smoke run. The app was stopped before completing the chart; app data and preferences were retained.
- No iOS build or Windows runtime test was run for this change. Their common resampling implementation changes, but platform performance was not measured.
- No Firebase upload was performed.
