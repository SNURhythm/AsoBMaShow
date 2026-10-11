# Bounded audio decoding — 2026-10-11

Decode through `sf_readf_double` in blocks of at most 4,096 complete frames,
then apply the existing `clamp(value, -1.0, 1.0) * 32767` conversion to each
sample. The final interleaved int16 PCM buffer, sample rate, channel count, and
metadata stay the same. Short reads still zero-fill the remainder, including
when the caller reuses an output buffer. Cancellation is checked between reads.

The floating-point scratch allocation is now at most 32 KiB per channel
(64 KiB for stereo), or smaller for clips shorter than a block. The final PCM
and any encoded input buffer remain allocated as before. This does not change
the audio codec, resample audio, or reduce decode precision.

## Compatibility and memory regression

The regression suite compares file and in-memory decoding sample-for-sample
against the prior one-shot floating-point algorithm:

- PCM WAV with mono, stereo, and three channels; 0, 1, 4,095, 4,096, 4,097,
  and 12,305 frames exercise empty input and block boundaries.
- Double-precision WAV includes positive/negative clipping and near-zero values.
- FLAC, Vorbis, and MP3 cover compressed decoders and partial final blocks.
- Truncated FLAC preserves the original output length and zero-filled tail.
- The bundled 60-second mono Ogg remains identical. With the final PCM reserved
  before observation, its largest C++ allocation fell from 21,168,000 bytes to
  32,768 bytes. The new scratch-budget test failed on the old decoder and passed
  after the change.

## Real audio benchmark

The user supplied a local corpus containing 18,036 WAV/Ogg files. Twelve files
(six of each format) were selected at encoded-size quantiles 0%, 25%, 50%, 90%,
99%, and 100%, among files between 4 KiB and 64 MiB. The selected files ranged from roughly
0.1 to 57.4 seconds and included mono and stereo audio. No source audio, file
names, paths, or raw benchmark logs are committed.

`scripts/benchmark_audio_decode.py` compiles both the actual decoder from commit
`69b050a4` and the current decoder with `-O2`. The baseline's public symbols are
renamed so both versions run in one executable using identical dependencies.
Each file is tested through ordinary file and in-memory entry points. After
warming both implementations, eight paired trials alternate their execution
order. Each trial starts with an empty output vector and checks every PCM
sample and the frame count, channel count, rate, and format against the baseline.
All **384 measured decodes** matched exactly.

This macOS arm64 run used Apple clang 21.0.0 and libsndfile 1.2.2. Target source
and baseline decoder were optimized; dependency libraries were reused from the
existing debug build. These are warm-cache local comparisons, not cold-storage
or mobile release-build measurements. Timings include open, decode, conversion,
output allocation, and close, but exclude fixture preparation, comparison, and
output destruction. Encoded bytes are prepared before timing the memory path.

### Speed

Median milliseconds over eight trials, for the longest file of each format:

| Audio | Input | Previous | Chunked | Time reduction |
| --- | --- | ---: | ---: | ---: |
| WAV, 57.0 s stereo | File | 12.553 | 11.637 | 7.3% |
| WAV, 57.0 s stereo | Memory | 10.985 | 10.243 | 6.8% |
| Ogg, 57.4 s stereo | File | 315.522 | 313.690 | 0.6% |
| Ogg, 57.4 s stereo | Memory | 312.332 | 312.516 | -0.1% |

Across the 12 per-file median ratios, geometric-mean time reduction was 2.0%
for file input and 1.6% for memory input. Ogg decoding was essentially unchanged;
the largest WAV improved modestly. The primary benefit is lower temporary
memory, not a general decoding-speed claim.

### Allocations

The observer counts ordinary C++ `new`/`new[]` requests. It excludes libsndfile
and codec-internal `malloc`, mapped memory, and total process RSS. Bytes below
are cumulative allocation requests per decode, including the final PCM buffer,
not a measured resident-memory peak.

| Audio / memory input | Previous bytes | Chunked bytes | Allocation count |
| --- | ---: | ---: | --- |
| WAV, 57.0 s stereo | 50,311,120 | 10,127,760 | 2 → 2 |
| Ogg, 57.4 s stereo | 50,618,880 | 10,189,312 | 2 → 2 |

The long clips allocate about 80% fewer C++ bytes. Allocation counts stayed the
same in every measured case: two for memory input and 15–54 for file input,
depending on path resolution. Scratch is allocated once and reused, with no
per-block allocation. Clips shorter than a block retain their previous scratch
size and receive no memory saving.

## Reproduce privately

Create a text file outside the checkout containing one local audio path per
line. Then run, without other builds or tests active:

```sh
cmake --build cmake-build-debug --target skin_sound_bundle_decode_tests -j 6
python3 scripts/benchmark_audio_decode.py --baseline 69b050a4 \
  --files-from /tmp/audio-files.txt --output /tmp/audio-decode-benchmark
```

The runner uses the existing Unix Ninja configuration, rebuilds its target
sources in the temporary output directory, and saves `commands.json` and
`benchmark.log` there. It does not copy or modify source audio and rejects an
output directory inside the repository. Individual decodes are capped at
64 Mi samples for this experiment. Do not commit the input list or output files.

## Verification

- Desktop `main` and all 20 targets directly compiling the decoder or allocation
  support rebuilt successfully.
- Full CTest suite: **481/481 passed** with `-j 6`.
- The scratch allocation regression was observed failing on the old decoder,
  then passing after chunking; all format/sample comparisons passed.
- The optimized benchmark completed all 384 measured comparisons successfully.
- The benchmark runner rejects an output directory inside the checkout.
