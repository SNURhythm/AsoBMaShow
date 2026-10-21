# Chart audio preparation and preview handoff

## Result

Skipping a redundant decoded-cache preflight for loose files reduced fully
prepared preview-to-gameplay audio handoff by **39–55%** across six local charts
at 44.1 and 48 kHz. The largest chart fell from 69.75 to 35.09 ms at 44.1 kHz
and from 64.95 to 38.58 ms at 48 kHz. Direct loading remained broadly similar,
particularly for Ogg-heavy charts where decoding dominates.

The previous implementation queried the decoded cache to choose between the
regular loader and archive extraction even for regular files. Cache lookup
validates file identity, including filesystem metadata. Regular files always
use `loadSound`, which performs its own cache lookup and validation. The change
bypasses only that redundant routing query; archive routing is unchanged.

## Method

The opt-in `scripts/benchmark_chart_audio_preparation.py` runner builds optimized
production sources (`-O2`, release third-party libraries) using the existing
headless Jukebox test backend. It parses real charts, disables visuals, and
measures three modes with fresh Jukebox/cache instances:

- Direct gameplay audio preparation without a preview cache.
- Fully prepared paged preview, cancellation/join, then gameplay handoff.
- Early handoff immediately after preview startup readiness, including cancellation/join.

Six loose charts from the private local iBMS corpus represent three WAV-heavy
and three Ogg-heavy folders. Folders were selected near audio-file-count
quantiles 0.25, 0.5 and 0.9 for each format, then the largest chart file in each
folder was selected. This is a representative local sample, not a random
population study. The chart reference counts were 133, 370, 177, 796, 931 and
1271 respectively. Names, asset paths and raw logs remain outside the repository.

Each rate/mode/chart has one warm-up and three measured rounds, with mode order
rotated between rounds: 108 measured loads per revision. Baseline and candidate
were separate sequential runs on the same machine, with warm OS file caches.
The candidate reused unchanged optimized objects and rebuilt Jukebox.cpp.
Medians below are milliseconds for gameplay preparation only; parsing, preview
preparation and cancellation are reported separately in the raw output.

| Rate | Case | Direct before | Direct after | Prepared handoff before | Prepared handoff after | Reduction |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 44100 | WAV 0 | 8.47 | 7.70 | 6.83 | 3.54 | 48% |
| 44100 | WAV 1 | 18.54 | 17.22 | 19.17 | 9.27 | 52% |
| 44100 | WAV 2 | 23.07 | 20.48 | 10.01 | 4.50 | 55% |
| 44100 | Ogg 3 | 254.26 | 245.78 | 44.92 | 22.58 | 50% |
| 44100 | Ogg 4 | 454.63 | 459.77 | 56.91 | 32.46 | 43% |
| 44100 | Ogg 5 | 478.48 | 479.42 | 69.75 | 35.09 | 50% |
| 48000 | WAV 0 | 10.40 | 10.43 | 7.09 | 3.86 | 46% |
| 48000 | WAV 1 | 19.76 | 19.19 | 19.42 | 9.60 | 51% |
| 48000 | WAV 2 | 23.31 | 22.80 | 9.97 | 5.10 | 49% |
| 48000 | Ogg 3 | 268.25 | 273.26 | 40.11 | 24.48 | 39% |
| 48000 | Ogg 4 | 518.27 | 520.43 | 63.40 | 32.61 | 49% |
| 48000 | Ogg 5 | 522.78 | 511.41 | 64.95 | 38.58 | 41% |

A separate five-second macOS `sample` of the baseline largest Ogg case at 48 kHz
showed 85 main-thread stack samples beneath the reconciliation cache query,
including canonicalization/stat work. These are stack counts, not a CPU-time
percentage. Worker stacks still showed Vorbis decoding and resampling.

## Correctness and limits

The benchmark asserts the same playable sound IDs in all modes, successful
handoff, and valid gameplay handles after clearing the cache and releasing the
preview source. Every fully prepared handoff added zero decoded frames.
Ogg case 3 contains missing assets and zero-byte placeholders: 767 of 796
referenced IDs are playable, and handoff retries 17 invalid decodes. Thus zero
successful redecoding does not imply zero decoder-open attempts. Direct-mode
cache counters are unobserved (`cache_observed=0`, printed as zero), not evidence of zero decoding.

Preview startup readiness can cover initial silence; it is not first audible
sound. Early handoff is scheduling-sensitive. Unlimited cache retention models
complete preview reuse; retained native PCM reached roughly 298 MB in case 4
and 212 MB in case 5, before additional output-rate PCM. No real audio device,
UI/rendering, visual loading, cold-disk behavior, mobile device or real archive
was measured. Timing differences in the small direct-load cases should not be
overinterpreted from three rounds.

## Reproduction and validation

The desktop `main` and `tests_audio` targets built successfully. All 34
`area_audio` tests passed (11.67 seconds wall time at `-j 6`). The final benchmark
counter-label change was compiled and smoke-tested with all three modes at
48 kHz. Read-only review found no production correctness issues.

Configure the existing desktop build and build `tests_audio` first, then supply
a private text file with one chart path per line:

```sh
python3 scripts/benchmark_chart_audio_preparation.py \
  --charts-from /tmp/private-charts.txt \
  --output /tmp/chart-audio-results --rounds 3
```

The output contains source hashes, compile/link commands and raw per-rate logs.
Use a separate `--sample-seconds 5` run for profiling to avoid contaminating the
timing comparison. Source charts and assets are read in place.

Next candidates are bounded native-PCM retention for large previews and reducing
48 kHz conversion work. Any further filesystem optimization must preserve asset
invalidation; archive extraction needs a separate representative benchmark.
