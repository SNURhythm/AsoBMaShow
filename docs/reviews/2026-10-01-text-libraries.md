# Text library experiment

## Result

simdutf is the strongest candidate for bulk UTF-8 validation. It also improves
skin text decoding on cache misses, but the existing cache removes most of that
work for unchanged labels. This experiment keeps the production utf8proc path
and dependency graph unchanged. BMS parser work is explicitly out of scope.

| Optimized CPU frame evaluation | utf8proc | simdutf | UTF8-CPP |
| --- | ---: | ---: | ---: |
| Unchanged text | 87.18 µs | 86.91 µs | 86.60 µs |
| Every value changes | 97.23 µs | 92.67 µs | 95.79 µs |

simdutf reduced changing-text evaluation time by 4.7% (4.56 µs/frame).
Unchanged-text ranges overlap: utf8proc 86.63–88.62 µs, simdutf 86.55–88.70 µs,
and UTF8-CPP 86.08–88.36 µs. Changing-text ranges were 96.18–98.38 µs,
92.41–95.27 µs, and 94.86–98.59 µs, respectively. These are synthetic CPU
measurements, not GPU frame times or mobile/FPS measurements.

## Individual operations

Median nanoseconds per operation, including the adapter's vector sizing for
conversion. Lower is better. Buffers are reused between iterations.

| Operation / input | utf8proc | simdutf | UTF8-CPP |
| --- | ---: | ---: | ---: |
| Validate 8-byte ASCII | 8.97 | 4.47 | 4.22 |
| Decode 8-byte ASCII | 14.56 | 16.60 | 7.48 |
| Decode 32-byte ASCII | 54.54 | 16.05 | 23.74 |
| Decode Korean title | 39.01 | 28.21 | 26.43 |
| Decode Japanese title | 44.00 | 30.05 | 29.02 |
| Decode mixed text, approximately 256 bytes | 374.92 | 207.84 | 228.74 |
| Validate approximately 64 KiB of mixed text | 60,599 | 6,098 | 70,156 |
| Decode approximately 64 KiB of mixed text | 82,727 | 39,279 | 54,285 |
| Decode truncated 32-byte input | 54.38 | 19.82 | 3,378.58 |

Bulk validation is about 9.9× faster with simdutf on this corpus. UTF8-CPP is
competitive for short valid text, but its checked conversion reports invalid
input with exceptions; those cost approximately 3.4 µs in the truncated-input
case. Its separate validator does not have that exception cost.

Malformed inputs are not uniformly faster with SIMD: when the first byte of
the 64 KiB input is invalid, utf8proc validation returns in about 2 ns while
simdutf takes about 6.08 µs. The simdutf conversion adapter also initializes its
output vector before conversion. These results argue for measuring actual
input distributions and retaining input limits, rather than assuming a
universal speed or security improvement.

## Method

- Baseline: `3c97f686`, including the decoded-text cache.
- Apple M1 Pro, macOS 26.5.1, Apple Clang 21.0.0.
- [utf8proc 2.11.3](https://github.com/JuliaStrings/utf8proc/releases/tag/v2.11.3),
  matching the application's installed version;
  [simdutf 9.2.1](https://github.com/simdutf/simdutf/releases/tag/v9.2.1), using
  its `arm64` implementation; and
  [UTF8-CPP 4.2.1](https://github.com/nemtrif/utfcpp/releases/tag/v4.2.1).
- Library objects and renderer translation units use `-O3 -g0 -DNDEBUG`.
  Existing LuaJIT, PCRE2, localization, and platform libraries come from the
  configured build and are identical across candidates. This is optimized
  renderer evaluation, not a complete application Release build.
- Only simdutf's UTF-8/UTF-32 features are enabled. The renderer experiment
  executable grows by 43,520 bytes with simdutf and 1,280 bytes with UTF8-CPP;
  these are local linked-binary differences, not app distribution sizes.
- A temporary source snapshot substitutes the cache-miss decoder. Every
  renderer object is rebuilt consistently for each backend. The scalar path
  for oversized/malformed strings and all glyph checks remain unchanged.
- Seven samples in seeded randomized backend order. The frame harness uses
  32 multilingual text objects, 100 warmup frames, and 2,000 measured frames.
  Every candidate emits 4,896,000 glyphs per measured case.
- Individual-operation loops use compiler barriers and consume their output.
  Correctness checks run before timing. Raw samples are in
  [the CSV](2026-10-01-text-libraries-samples.csv); its `ns` column includes
  frame measurements converted to nanoseconds.

## Correctness

Each candidate passed:

- All 1,112,064 Unicode scalar values, with independently encoded expected data.
- Fixed examples covering NUL, CRLF, combining marks, emoji, BOM, noncharacters,
  and the maximum scalar value.
- 3,598 malformed cases with varying prefix lengths across SIMD boundaries.
- 50,000 seeded random byte strings compared against utf8proc's acceptance
  and decoded output.
- The existing cache tests and full renderer command/regression tests.
- The correctness corpus and cache tests under AddressSanitizer and
  UndefinedBehaviorSanitizer, with the library objects instrumented as well.

No acceptance differences were observed. This is bounded compatibility and
memory-safety testing, not evidence that replacing utf8proc fixes a security
vulnerability. Normalization and case folding remain utf8proc responsibilities.

## Reproduce

Use a configured Ninja build with `skin_draw_command_tests` already built:

```sh
python3 scripts/benchmark_text_libraries.py
```

The script downloads hash-checked releases and writes snapshots, binaries,
build metadata, and `results.json` under `/tmp/asobmashow-text-libraries`.
It never modifies production sources or installs a candidate dependency.
Use `--output` for another external directory, `--samples` to change the sample
count, or `--skip-build` to remeasure the existing binaries. The build manifest
records compiler identities and effective commands for future runs.

The next useful experiment is bulk validation in Lua file-reading helpers.
Keep normalization with utf8proc, and verify real workloads before adopting a
second library solely for the modest cache-miss rendering gain.
