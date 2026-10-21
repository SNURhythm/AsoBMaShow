# Android audio preparation

## Gameplay constraint

All gameplay audio remains fully decoded and resident before play. Native PCM
and output-rate PCM ownership, preview retention policy, mixer callbacks and
resampling are unchanged. No streaming, lazy decoding, eviction or background
loading was added to gameplay.

## Changes

Ordinary POSIX files now use one `stat` snapshot for decoded-cache identity,
including device, inode, size, modification time and change time. This preserves
symlink following and detects replacement or rewriting with restored mtime.
Android SAF and archive-reference identities retain their existing paths.

Loose chart sound resolution now keeps a bounded, preparation-local directory
name filter. It starts only after a missing filename, preserves extension order,
and validates positive candidates through the original existence check. Directory
identity, mtime and ctime invalidate snapshots. Enumeration is limited to 16,384
entries in total, including refreshes, 4,096 names per snapshot and 32 directories.
Unreadable, oversized, Unicode-containing and unstable directories fall back to
normal lookup. Windows short-name aliases, relative paths, SAF and archives also
retain their original lookup. Cancellation is checked while enumerating. These
snapshots contain names only and are destroyed before gameplay.

## Device and method

The connected Samsung Galaxy S20 FE (SM-G781N), Android 13, ran headless probes
linked against the built arm64 production library at 48 kHz with six decode
workers. Six real loose charts were staged on emulated external storage, plus an
existing 44 MB ZIP containing 599 entries and 94 charts (about 71 MB expanded).
The ZIP test selects one chart with 370 sound references. This is a real archive
measurement, not evidence for tens/hundreds-of-gigabytes archive performance.
Archive algorithms were not changed; no full-archive payload scan was added.

The benchmark rotates direct loading, fully prepared preview handoff, and early
preview handoff, with one warm-up and three measured rounds per mode. Baseline
and candidate runs were sequential, with warm filesystem caches and normal
Android scheduling/DVFS; small differences are not reliable speedup claims.
Native probes exclude UI, visuals, SAF, hardware audio and realtime playback.

A focused largest-Ogg-chart probe attributed 11.63 seconds of an 11.86-second
prepared handoff to filename resolution; reconciliation took 0.24 seconds.
A separate probe found 100 existing-file stats took about 3 ms, while 100 missing
names took about 390 ms. Listing that directory's 1,338 names took 283 ms.
Charts requesting `.wav` when `.ogg` exists repeatedly paid that missing-file
cost. Parallelizing these lookups did not improve the measured handoff.

## Results and correctness

The initial candidate completed all seven cases (84 loads including warm-ups).
After strengthening directory invalidation to include inode and ctime, the final
library repeated both affected Ogg cases and the ZIP (36 loads). The table uses
that final run, compared with baseline commit `4afc15de`. Values are median
milliseconds for audio preparation, excluding preview preparation and joining.

| Case | Direct before | Direct after | Prepared handoff before | Prepared handoff after | Early handoff before | Early handoff after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Ogg 3, 796 references | 5346.46 | 903.98 | 5583.26 | 286.28 | 5931.30 | 674.23 |
| Ogg 5, 1271 references | 12607.10 | 1667.14 | 11021.00 | 466.93 | 12066.10 | 1610.01 |
| ZIP, 370 references | 198.67 | 213.42 | 305.56 | 125.15 | 358.81 | 198.69 |

Prepared handoff improved about 95–96% in the two affected Ogg cases. WAV-heavy
cases were broadly similar in direct load. Ogg case 4 contains Unicode names and
intentionally uses the original lookup; its initial-candidate prepared median
was 7.88 seconds versus 7.41 seconds baseline. The filter does not solve that
case. The ZIP result exercises existing archive routing and the simpler identity
check; archive performance conclusions remain limited to this one small ZIP.

Every mode preserved matching playable sound IDs and gameplay ownership after
clearing the cache and releasing the preview. Every fully prepared handoff added
zero decoded frames. Case 3 has 767 playable IDs: missing references and 17
zero-byte placeholders explain the lower count and repeated invalid decode
attempts. Direct-mode cache statistics are unobserved, not evidence of no decode.
Early handoff may decode remaining audio during preparation, before gameplay.

Desktop `main` and `tests_audio` builds passed; all 34 audio tests passed in
11.65 seconds at `-j 6`. The final restored-directory-mtime regression was also
rebuilt and passed after tightening its setup. The signed Android release build
passed, using explicitly approved versionCode 2 for the device update. Review's
directory-invalidation finding was fixed; the helper adds no realtime work.

The approved versionCode 2 APK was installed over the existing app without
clearing data. On-device smoke testing reached music selection, preview, chart
viewer and autoplay. Screenshots showed notes progressing and combo increasing;
logs showed Vulkan rendering and successful AAudio stream open/start, with no
fatal exception in the app log. This verifies playback startup and progress,
not subjective listening quality or a long-duration glitch-rate measurement.
Temporary benchmark inputs and binaries were removed from the phone.

## Other investigated options

- A 64 MiB preview-cache cap reduced retained native PCM in one desktop chart
  from 298 MB to 67 MB, but caused about 59.8 million frames to be decoded again
  during handoff (about 80% of the chart). It was rejected. No production memory
  cap or eviction policy changed.
- A private 44.1/48 kHz resampler specialization matched 1,106,181 output samples
  across 147 rate/channel cases. Device stereo gains were only about 0–2% in the
  exploratory comparison, so the added specialization was rejected.
- Direct Ogg loading is covered by the device benchmark. The earlier Vorbis
  scratch-memory reduction remains; no speculative decoder replacement was added.

## Reproduction

Build the Android release library, then provide a private text file containing
one absolute device chart path per line. The runner requires an explicit device
and matching build/library directories. It records commands, library hashes,
device fingerprint and raw output; its temporary device binary directory is
removed on exit. It neither installs an APK nor uploads a distribution build.

```sh
scripts/android_firebase_deploy.sh --build-only
python3 scripts/benchmark_android_chart_audio.py \
  --device DEVICE_SERIAL \
  --build-dir android/app/.cxx/RelWithDebInfo/BUILD_ID/arm64-v8a \
  --libraries android/app/build/intermediates/merged_native_libs/restricted_file_accessRelease/mergeRestricted_file_accessReleaseNativeLibs/out/lib/arm64-v8a \
  --charts-from /tmp/private-device-charts.txt \
  --output /tmp/android-chart-audio-results
```

The desktop runner now uses the same standalone benchmark source. Its optional
`--cache-bytes` argument is diagnostic only and does not alter production policy.
Private charts, paths and raw logs are not committed.
