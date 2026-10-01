# Production UTF-8 adoption

The game now uses simdutf 9.2.1 for skin file validation, skin text validation,
and decoded-text cache misses. The existing cache, size limits, glyph checks,
and malformed/oversized renderer fallback are preserved. utf8proc continues
to handle normalization and the scalar diagnostic path. BMS parsers were not
changed.

## Measured results

Seven samples, median time in microseconds; lower is better. The final samples
were taken after this task's builds and tests finished.

| Workload | Previous decoder/validator | Production | Time reduction |
| --- | ---: | ---: | ---: |
| Lua `file_count_lines` | 169.28 | 131.16 | 22.5% |
| Lua `file_read_lines` | 182.31 | 146.19 | 19.8% |
| CPU skin evaluation, every text value changes | 93.25 | 90.91 | 2.5% |
| CPU skin evaluation, unchanged text | 84.22 | 83.85 | Effectively tied |
| Validate approximately 64 KiB, utf8proc baseline | 58.16 | 5.94 | 9.8× throughput |

The file helpers read an actual 65,800-byte file through the Lua host and virtual
filesystem, including UTF-8 validation and line processing. The file contains
1,400 repeated multilingual lines. Reads use a warm filesystem cache; Lua string
interning can reduce repeated-line materialization cost. These measurements do
not predict cold storage performance or files containing distinct lines.

Ranges for line counting were 163.80–179.93 µs before and 129.35–131.72 µs after;
line reading was 176.95–188.19 µs before and 143.38–148.47 µs after. The changing
text frame ranges were 91.81–95.95 µs and 90.01–91.81 µs. Unchanged text ranges
overlap (83.48–87.48 µs and 83.40–89.71 µs). This is synthetic desktop CPU work,
not a measurement of mobile frame rate or GPU rendering.

The frame comparison restores only the prior utf8proc cache-miss decoder. The
file-helper comparison restores only the exact manual UTF-8 validator from
`f380b5bf`; other code and dependencies remain identical between each pair.
These differ from the earlier experiment's library-only comparison.

## Implementation and build integration

`src/text/Utf8.cpp` owns the checked validator/converter. Its interface exposes
no simdutf types. Conversion creates live output objects before writing, clears
partial output on errors, and handles an empty default `string_view` explicitly.
Cache hits still share immutable decoded scalars across frames.

The SHA-verified upstream release was amalgamated with only UTF-8 and UTF-32
features. Its generated implementation is included as `.cpp.inc` by one ordinary
source file, so both CMake and Xcode's synchronized source group compile it once.
Runtime CPU dispatch and the portable fallback are retained. Provenance, exact
hashes and regeneration instructions are in `src/text/simdutf/README.md`; the
MIT notice is bundled at `assets/legal/simdutf.txt`.

## Validation

- Desktop application and all test targets built successfully.
- All 402 CTest cases passed with `ctest --test-dir cmake-build-debug --output-on-failure -j 6`.
- The production helper passed all 1,112,064 Unicode scalars, 3,598 malformed
  boundary cases, and 50,000 seeded random strings against utf8proc.
- Lua integration checks cover malformed sequences after long prefixes,
  embedded NUL, BOM, supplementary characters, and CR/LF handling.
- Production Unicode and cache tests passed AddressSanitizer and
  UndefinedBehaviorSanitizer with ARM64 dispatch and forced portable fallback.
- The new implementation compiled for iOS ARM64 device and simulator, Intel
  macOS, and Android ARM64/x86-64 using NDK 28.2.13676358. These were source
  compilation checks; full mobile app builds and device benchmarks were not run.

## Reproduce

Use the configured desktop Ninja build with `skin_draw_command_tests` and
`lua_skin_host_modules_tests` built, and keep `f380b5bf` available in local git
history for the historical file-validator comparison:

```sh
python3 scripts/benchmark_text_libraries.py --output /tmp/asobmashow-text-adoption
python3 scripts/benchmark_skin_file_helpers.py --output /tmp/asobmashow-text-adoption
```

Each script supports `--skip-build` to remeasure its existing binaries. Run them
without competing builds/tests. Build commands, source hashes and results are
saved under the output directory. The second script uses the first script's
source snapshot and optimized dependency objects. `--benchmark-files` is also
available directly on the host-module test executable for local profiling.

Measurements used Apple M1 Pro, Apple Clang 21.0.0, and macOS 26.5.1. Library and
renderer/host translation units use `-O3 -g0 -DNDEBUG`; other configured LuaJIT,
PCRE2, localization and platform libraries are held constant. This is not a
complete Release application build.

Raw data: [operations and frames](2026-10-01-utf8-adoption-samples.csv),
[file helpers](2026-10-01-utf8-adoption-files.csv). Frame rows in the first CSV
are nanoseconds; the file-helper CSV uses microseconds.
