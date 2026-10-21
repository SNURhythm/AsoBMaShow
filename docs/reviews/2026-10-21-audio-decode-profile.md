# WAV and Vorbis decode profiling — 2026-10-21

The full-buffer decoder now reads PCM16 WAV directly into its final PCM buffer
and preserves the historical amplitude conversion with integer arithmetic.
Vorbis uses native float scratch samples, then promotes to double for the
existing PCM conversion. Both paths preserve every output sample, metadata,
limits, cancellation checks and short-read silence. Other formats retain the
double path. No hardware codec or dependency was added.

## Measurements and method

Selected seven WAV and seven Ogg files from a user-provided local chart corpus,
at encoded-size quantiles 0%, 25%, 50%, 75%, 90%, 99% and 100%. Selection examined
paths and file sizes only, among nonempty files no larger than 64 MiB:
3,316 WAV and 14,702 Ogg candidates. Only the selected audio was read. The WAV
sample spans 442 bytes to 10,062,370 bytes; Ogg spans 3,453 to 2,663,879 bytes.
All selected WAVs are PCM16 and all selected Oggs are Vorbis. Private file names,
paths, audio and raw logs are not committed.

`scripts/benchmark_audio_decode.py` compiles the actual baseline decoder from
`cc7bf095` and the current decoder with Apple clang 21.0.0, `-O2`, on macOS arm64.
Both use the same **release** libsndfile 1.2.2 and codec dependencies from the
existing vcpkg installation (`--release-audio-libs`). Other dependencies,
including archive/decompression libraries, keep their existing debug build.
The earlier October 11 benchmark used debug audio dependencies; its timings
are not directly comparable.

Each asset is measured from its ordinary file, prepared memory, and a temporary
single-entry deflated ZIP. ZIP creation and loading the memory input are outside
timing. After warming both implementations, eight paired trials alternate their
order. Each decode starts with an empty output vector; timing includes path
resolution, opening, allocation, decoding, conversion and close, but excludes
output comparison/destruction. All **672 measured decodes** match the baseline's
PCM and metadata exactly, as do the warmups and separate cache checks.

These are warm-cache per-asset desktop measurements, not complete chart-load,
cold-storage, mobile, or large-archive benchmarks. The selected sample is not
weighted by chart frequency. No archive algorithm changed, and the synthetic
ZIP cases do not establish performance for a real multi-gigabyte archive.

## Before and after

Median milliseconds for the largest selected asset of each format, without
per-stage profiling clocks:

| Asset / input | Baseline | Current | Time reduction |
| --- | ---: | ---: | ---: |
| WAV, 57.0 s stereo / file | 3.777 | 1.648 | 56.4% |
| WAV, 57.0 s stereo / memory | 2.572 | 0.503 | 80.4% |
| WAV, 57.0 s stereo / ZIP | 56.092 | 53.875 | 4.0% |
| Ogg, 57.4 s stereo / file | 126.117 | 129.249 | -2.5% |
| Ogg, 57.4 s stereo / memory | 129.562 | 125.832 | 2.9% |
| Ogg, 57.4 s stereo / ZIP | 139.039 | 141.738 | -1.9% |

Geometric-mean reductions across each format's seven per-file median ratios:

| Format | File | Memory | ZIP |
| --- | ---: | ---: | ---: |
| WAV | 36.3% | 72.7% | 3.3% |
| Ogg | -1.4% | 1.6% | -1.2% |

The shortest WAV is dominated by opening and path work, with no meaningful file
speed improvement. Vorbis has no demonstrated throughput improvement: its small
changes vary in direction across input paths. Its retained benefit is halving
scratch memory, not a decoding-speed claim.

## Four-stage profile

A separate `--profile` run adds clocks only to the current decoder. These clocks
are absent from normal builds and from the before/after run above. Stage times
are wall times, not CPU samples; they omit allocation, path resolution and close.
`input_ms` measures the explicit archive read/extraction. For ordinary files,
libsndfile's own I/O is included in `open_ms` and `read_ms`; the memory path helps
separate codec work from filesystem overhead.

Selected current-decoder medians, milliseconds:

| Asset / input | Total | Archive input | Open | Read/decode | Convert |
| --- | ---: | ---: | ---: | ---: | ---: |
| Largest WAV / memory | 0.583 | 0 | 0.005 | 0.224 | 0.192 |
| Largest WAV / ZIP | 54.263 | 53.664 | 0.012 | 0.180 | 0.179 |
| Smallest Ogg / memory | 0.242 | 0 | 0.209 | 0.029 | 0.0004 |
| Largest Ogg / memory | 123.676 | 0 | 0.406 | 121.557 | 1.565 |
| Largest Ogg / ZIP | 138.577 | 14.428 | 0.408 | 122.103 | 1.589 |

The findings support different priorities:

- PCM WAV conversion was worth optimizing. In the initial bundled/generated
  profile, conversion consumed about 60% of a 30-second stereo WAV's in-memory
  decode time.
- Decoder opening dominates very short Oggs; repeated opens are expensive even
  when there are few samples. Long Oggs are dominated by codec decoding.
- ZIP extraction can dominate WAV loading. Decoder speed alone cannot remove
  that cost.
- Existing decoded-cache reuse already skips both decoding and extraction.
  In the unprofiled run, warm loads of the largest WAV/Ogg took approximately
  0.033/0.035 ms from files and 0.074/0.079 ms for archived assets. Counters stayed
  at one decoder open and one archive read for archived assets across nine
  loads. This measures the existing cache; it is not a new cache optimization.
  The cache benchmark budget holds one selected asset; it does not test churn
  under the application's default cache budget.

Existing archive batch loading, source identity checks, cache policy and
parallel scheduling were retained. These measurements do not justify a codec
replacement or new hardware backend. In particular, hardware acceleration
would not eliminate short-file setup or archive extraction costs.

## Implementation and compatibility

For PCM16 in WAV, WAVEX and RF64, the old normalized-double conversion is
`trunc(sample * 32767 / 32768)`. For every int16 value this equals
`sample - (sample > 0) + (sample < 0)`. Applying that transform after
`sf_readf_short` removes the scratch allocation without changing amplitudes.
Apple clang's optimization remarks confirm CPU vectorization of this loop
(width 8, interleave 4); no architecture-specific intrinsics are required.

libsndfile's Vorbis float reader copies the native decoded floats. Its double
reader only widens those same values. Reading float and retaining double
arithmetic for scaling therefore preserves the original rounding. Using float
arithmetic for the final scaling would not offer that guarantee.

At a 4,096-frame block size, stereo PCM16 WAV scratch falls from 64 KiB to zero;
stereo Vorbis scratch falls from 64 KiB to 32 KiB. The final PCM buffer is
unchanged. The observer measures C++ allocations, not codec-internal `malloc`
or process RSS. The full-buffer loader is shared by gameplay, decoded-cache
loads and offline rendering; the separate progressive listening decoder is
outside this change.

## Verification

- New PCM16 and Vorbis scratch-budget assertions failed against the original
  decoder, then passed with the optimized paths.
- Exhaustive PCM16 values, existing block-boundary cases, WAV/WAVEX/RF64,
  big-endian PCM16, 8/24/32-bit PCM, float/double WAV, FLAC, Vorbis, MP3 and
  truncated FLAC compare against the independent one-shot double oracle.
- `cmake --build cmake-build-debug --target tests_audio main -j 6` passed.
- `ctest --test-dir cmake-build-debug -L '^area_audio$' --output-on-failure --no-tests=error -j 6`:
  **34/34 passed**, 11.94 seconds wall time.
- Unprofiled and profiled real-corpus benchmark runs completed with exact PCM
  comparisons. The standalone profiling flag also passed a compile check.
- Read-only code review found no blocking correctness issues; its profiling
  preprocessor-guard finding was corrected.

## Reproduce

Put one selected local audio path per line in a private file outside the repo.
Run with other builds/tests idle:

```sh
python3 scripts/benchmark_audio_decode.py --baseline cc7bf095 \
  --files-from /tmp/audio-files.txt --release-audio-libs \
  --output /tmp/audio-decode-comparison
python3 scripts/benchmark_audio_decode.py --baseline cc7bf095 \
  --files-from /tmp/audio-files.txt --release-audio-libs --profile \
  --output /tmp/audio-decode-profile
```

Use the first run for before/after percentages and the second for stage
attribution. Each output directory contains the private command manifest and
raw log. The runner rejects output directories inside the checkout. It uses the
existing Unix Ninja target configuration; no release application build or
mobile deployment is performed.
