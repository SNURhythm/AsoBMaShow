# Archive audio preparation

## Result

Prepared preview-to-gameplay handoff for a real ZIP on the connected Galaxy S20
FE improved from **71.84 ms to 39.89 ms (44%)**. A desktop sparse ZIP64 fixture
with 100,003 entries improved from **32.91 ms to 0.13 ms**. All gameplay audio
remains fully decoded and resident; playback callbacks, resampling, and PCM
retention policy are unchanged.

## What changed

Sound resolution previously copied the entire cached archive entry list and
built two additional filename maps for each chart load. It now resolves only
that chart's requests through `resolveArchiveEntriesWithExtensions`, reusing the
existing index. Requested sound IDs, original order, exact/case-insensitive
matching and extension fallback remain supported. The extra preparation state
scales with chart references rather than unrelated archive entries. Existing
visual lookup paths are unchanged.

Cached archive handoff previously performed `Contains` followed by `Load` for
each sound, repeating indexed filename resolution and source metadata checks.
It now captures one decoded-cache archive snapshot, finds prepared PCM against
that snapshot, and attaches the same native/output buffers. It checks source
identity again after all preparation workers join. A source change fails
preparation and retires partially attached sounds. Cancellation still prevents
playback from starting. This adds no archive access during gameplay.

This does not change extraction or compression algorithms, eliminate the first
index build, or reduce metadata reads required to open an archive for extraction.
Direct decoding of the small real ZIP remains broadly similar.

## Real-device measurement

Device: Samsung Galaxy S20 FE (SM-G781N), Android 13, 48 kHz, six audio workers.
Input: existing 46,111,783-byte ZIP, 599 entries, 94 charts; selected chart has
370 sound references. This is the same private archive used in the preceding
[Android investigation](2026-10-21-android-audio-preparation.md).

The baseline is the production library at `5deb6997`; the candidate is the built
Android production library with this change. A paired baseline/candidate run
used five measured rounds plus one warm-up per mode, rotating modes between
rounds. The app was stopped during these headless runs. OS caches were warm;
Android scheduling and DVFS were uncontrolled. Timing is audio preparation only,
excluding preview preparation and cancellation/join.

| Mode | Before (ms) | After (ms) |
| --- | ---: | ---: |
| Direct load | 162.85 | 170.10 |
| Fully prepared preview handoff | 71.84 | 39.89 |
| Early preview handoff | 106.76 | 92.66 |

All modes retained the same 370 playable IDs. Fully prepared handoff decoded
zero additional frames. Native cache retention remained 33,711,136 bytes, with
additional output-rate PCM unchanged. An earlier five-round candidate run also
passed; the table uses the paired comparison. The modest direct-load difference
is not evidence of an improvement or a conclusive regression from this sample.

## Large-archive scale check

The opt-in `scripts/create_archive_audio_scale_fixture.py` creates a ZIP64
archive containing a valid sparse 128 GiB zero-filled member that the chart does
not reference, 100,000 unrelated empty entries, one small chart, and one stereo
WAV referenced twice using an extension/case alias. It writes no giant payload
and checks only the selected WAV. The zero member's CRC is precomputed by
logarithmic CRC combination, not by scanning its contents.

Measured fixture sizes:

- Logical archive: 137,451,930,487 bytes.
- Allocated disk space on this APFS run: 20,340,736 bytes.
- Entries: 100,003.
- Central directory and footer: 7,800,325 bytes.
- Selected encoded WAV: 176,444 bytes; native PCM: 176,400 bytes.

Optimized desktop benchmarks reused unchanged source objects and rebuilt
Jukebox/AudioWrapper. All other source hashes matched. The baseline has the same
archive preparation behavior as `5deb6997`. Each process used one warm-up and
three measured rounds per mode; the table compares the final baseline/candidate
repeat after compilation had finished. An earlier run under memory pressure was
excluded. Cold index construction is outside these warm preparation medians.

| Mode | Before (ms) | After (ms) |
| --- | ---: | ---: |
| Direct load | 42.60 | 1.91 |
| Fully prepared preview handoff | 32.91 | 0.13 |
| Early preview handoff | 33.16 | 1.99 |

Both referenced IDs remain playable and alias the same sound. Prepared handoff
adds zero decoded frames. A separate macOS I/O probe counted source reads through
`fread`, `read` and `pread`, filtered by source device/inode. Across chart parsing
and six warm-up/measured loads it counted **72,867,095 source bytes**, including
repeated metadata reads, in both revisions. It counted **zero bytes from the
unselected 128 GiB member**. Its mapping-fallback hook was never invoked. These
are instrumented logical read counts, not physical disk traffic; timings above
come from uninstrumented runs.

The large speedup addresses duplicate index processing, not faster media I/O.
The fixture validates scale behavior and bounded payload access, but does not
replace timings from a representative real multi-hundred-gigabyte collection.

## Verification and reproduction

Desktop `main` and `tests_audio` built; all 34 audio tests passed in 11.91 seconds
at `-j 6`. ZIP and 7-Zip tests cover cached PCM reuse and source replacement with
preserved size/mtime. A deterministic test changes an archive during mixed
cached/uncached preparation, checks failure and removal of attached resources,
and then verifies a clean retry. Existing ownership tests and benchmarks verify
that gameplay retains PCM after releasing preview/cache owners. Read-only review
found no material issues.

The Android signed release build passed using the already approved versionCode
2. The updated APK was installed without clearing user data. Native device
benchmarks use the production library and exclude UI, visuals, SAF and hardware
audio timing. A separate app smoke test reached preview and autoplay, with
visible note/combo progression, Vulkan rendering and successful AAudio startup.
No fatal exception appeared in the app log. This is not a subjective listening
or long-duration glitch-rate test. Owned benchmark inputs/binaries were removed
from the phone; user charts and app data were preserved.

For the scale fixture, use a sparse-capable filesystem and an output path that
does not exist:

```sh
python3 scripts/create_archive_audio_scale_fixture.py \
  --output /tmp/archive-audio-scale.zip > /tmp/archive-audio-scale.json
printf '%s\n' /tmp/archive-audio-scale.zip/song/chart.bms > /tmp/archive-audio-scale-charts.txt
python3 scripts/benchmark_chart_audio_preparation.py \
  --charts-from /tmp/archive-audio-scale-charts.txt \
  --sample-rates 48000 --rounds 3 --output /tmp/archive-audio-scale-results
```

Use `scripts/benchmark_android_chart_audio.py` for real archives already staged
on the device, following its explicit device/build/library arguments. Do not
copy the sparse fixture to the phone with a tool that expands sparse holes.
Private corpus paths, raw measurements and throwaway profiling instrumentation
remain outside the repository.
