# ZIP highlight preview startup

The first selection of a chart in a ZIP was still waiting for most of its audio
extraction. Duration checks ran after complete ZIP inflation and CRC validation,
even when a short header could prove that an old sound had already ended.
Cached library indexes did not avoid that payload work.

The user reported approximately one second on iOS with a 20 GiB ZIP, on the first
selection of each chart. That archive was unavailable locally. A smaller real
ZIP reproduced the scheduling problem with a warm archive index and fresh audio
cache. Sampling showed ZIP inflation and CRC checks dominating the CPU work;
highlight scoring took approximately 0.12 ms.

## Change

`probeZipEntries` borrows the cached index and one central-directory reader for
the chart. It reads selected entry metadata with a 128 KiB payload-I/O limit per
entry. Stored entries support random access; deflated entries inflate only the
requested prefix, capped at 64 KiB. It never scans unrelated payloads or copies
the whole archive index. Cancellation and source identity are checked before
publishing the batch's duration decisions.

WAV/FLAC duration headers and stored Ogg head/tail reads can exclude expired
voices before extraction. Large deflated Ogg files remain unknown when their
last page lies beyond the prefix limit. A subsequent user-requested adjustment
sets their lookback to 60 seconds; other unknown formats retain 15 seconds.
Container detection uses the header even when an Ogg asset is named `.wav`.
The bound stays fixed for each past instance; future instances remain scheduled.

The existing indexed extraction pipeline now handles opening/overlapping sounds
first, then the remaining gameplay warm-up. Each full payload is still extracted
once and CRC-validated. Header/prefix reads add bounded duplicate I/O; they do
not replace full payload integrity checks. Native PCM retention and the 8 MiB
deferred-encoded budget remain unchanged. Other archive formats keep their
existing extraction/probing route.

## Local measurements

Optimized headless desktop benchmark, 48 kHz, one warm-up plus three measured
rounds per mode, warm filesystem/index, fresh native cache for each mode. The
real ZIP is 46,111,783 bytes with 599 entries; the chart references 370 sounds.
The benchmark starts at the actual scored highlight and trims leading silence.
It also verifies unchanged playable IDs and zero repeat decoding after a fully
prepared preview-to-gameplay handoff.

| Measurement | Before | After |
| --- | ---: | ---: |
| Median early preview startup | 106.0 ms | 21.4 ms |
| Sounds decoded at early cancellation | 360 | 23 |
| Median full preview preparation | 109.6 ms | 116.8 ms |
| Median loose-file early preview startup | 8.4 ms | 8.7 ms |

This is an approximately 80% reduction in local ZIP startup time. Total
preparation increased modestly because metadata probes and a second batch setup
add work. These are preparation timings, excluding UI debounce, hardware audio
and the preview fade. They do not establish the timing of the user's iPhone or
20 GiB archive.

A sparse 128 GiB ZIP with 100,004 entries completed highlight preparation and
all handoff modes. An instrumented run covering parsing, warm-up and one measured
round counted 18,239,580 source bytes through `pread`, `read` and `fread`, with
**zero bytes from the unselected 128 GiB payload**. The fixture uses stored
entries, so payload extraction did not use mmap. Its allocated size was about
18 MiB. This validates archive-scale access patterns, not physical disk traffic
or representative iOS latency.

## Verification

The expired ZIP regression now blocks allocation of the full encoded sound and
requires audible highlight startup before releasing it. It failed before the
change and passed afterward. Additional checks cover stored/deflated WAV,
stored Ogg tail reads, large deflated Ogg fallback, byte/prefix limits,
cancellation, source replacement, future reuse and gameplay cache ownership.

Desktop `main`, `tests_audio` and `tests_archives` built successfully. All 53
selected audio/archive tests passed at `-j 6`; the final focused preview test also
passed after the portable source-replacement fixture adjustment. A valid deflate
fixture with an empty-block prefix also verifies repeated reads after input-budget
exhaustion; the final three focused preview/archive tests passed. Independent
read-only review found no actionable issues. The unsigned iOS device build passed
using `scripts/ios_firebase_deploy.sh --build-only --skip-init`, including the
final incremental check. No deployment or on-device timing measurement was made.

## Reproduction

Use a private file containing one virtual chart path per line, such as
`/path/collection.zip/song/chart.bms`:

```sh
python3 scripts/benchmark_chart_audio_preparation.py \
  --charts-from /tmp/private-charts.txt --highlight \
  --sample-rates 48000 --rounds 3 --output /tmp/zip-highlight-results
```

The benchmark records source hashes, commands and measurements outside the
checkout. Use its `--sample-seconds` option only for a separate profiling run;
profiling distorts timing. Private chart paths and raw profiling artifacts are
not committed.

## Long PCM WAV overlap correction

The author's public [Giselle download](https://manbow.nothing.sh/event/event.cgi?action=More_def&event=123&num=262)
reproduced missing audio in the WAV pack at the 34.4537-second highlight; the
Ogg pack did not. `mdf1.wav` is a 52.203-second PCM backing track, already
32.891 seconds into playback at that point. Its LIST metadata follows the audio
payload. The bounded deflate reader could not reach that trailing chunk, so the
probe returned unknown and the 15-second fallback incorrectly excluded it.

For supported PCM WAV headers, the probe now bounds duration by treating every
byte from the first data payload to the RIFF/file end as possible PCM. Trailing
metadata and additional data chunks can only make that bound conservative. The
initial format controls the frame size and rate; libsndfile ignores subsequent
format chunks. The existing RIFF/file-size and format checks remain. No payload
or trailing metadata read is required, and ZIP probe budgets are unchanged.

An optimized headless comparison rendered 20 seconds from the scored highlight
at 48 kHz in 4,096-frame blocks, while decoding was still in progress. The
reference played the same chart from its beginning to that timestamp. Before
the fix, the WAV highlight retained only 56.2% of the reference energy. After
the fix, both WAV and Ogg matched within floating-point mixing order error
(maximum absolute difference below 0.0000003). This checks mixed source output,
not an iPhone audio recording.

Regression coverage includes a header-only 128 MiB WAV with an unreadable tail
(at most 128 requested bytes), a tagged long WAV inside stored and deflated ZIPs,
placed and BGM lookbacks, and future reuse of an excluded past sample across
buses. Desktop `main` built and all 34 audio tests passed.
The unsigned iOS device build passed, and independent review found no blocking
correctness issues. No deployment or on-device verification was performed.
