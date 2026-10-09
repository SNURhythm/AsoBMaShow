# Windows gameplay frame indexes

Owning gameplay sessions now prepare the renderer's object and property indexes
once, after their immutable model reaches its final address. Previously every
frame allocated and sorted the five property registries, the disabled-object
list and the object list. The prepared index contains structural pointers and
validation flags only: callback results, timers, visibility, note projection,
text, interaction geometry and draw commands are still evaluated each frame.

The renderer checks both the model address and session identity before using
the prepared index. Standalone evaluators and non-owning test sessions retain
the original transient-index behavior, including validation after model edits.

## Reproduction

Build with MSVC Release, then run the existing production-session fixture:

```powershell
cmake --build --preset release-windows --target gameplay_skin_loading_benchmark_tests --parallel 6
./cmake-build-release-visual-studio/Release/gameplay_skin_loading_benchmark_tests.exe --acceptance-report --skin F:\beatoraja\skin\LITONE12 --entry Play/play7.luaskin --entry-identity litone12-play7 --frame-samples 20000
```

`--frame-samples` is optional and only valid with `--acceptance-report`.
It excludes 256 warmup frames and reports the median, p95, command count and
individual nanosecond samples. It measures `PlaySkinSession::prepareFrame`
with synthetic chart state and default skin settings, advancing visual time
by 500 microseconds per frame. Package setup, state copying, diagnostic
inspection and destruction of the returned frame are outside each sample.
Texture and movie devices are fakes. GPU submission, presentation, BGA playback
and a real chart's projected notes are absent; this is not an in-game FPS test.

The local third-party skin is copied into temporary test storage and is not
modified or redistributed. On this Windows machine, copying, snapshotting and
validating the full 811 MB package takes several minutes before measurement.

## Exploratory timing

One before/after process pair used MSVC x64 Release `/O2` and 20,000 samples
per executable. The baseline production code is `8e62f73b`, with the same
frame-measurement loop added to its fixture. This LITONE12 model contains
1,166 objects, 1,154 destinations and 1,097 property bindings.

| CPU preparation | Before | Prepared index |
| --- | ---: | ---: |
| Median | 1,015.7 us | 950.4 us |
| p95 | 1,473.8 us | 1,396.7 us |
| Commands across measured frames | 3,917,218 | 3,917,192 |

The median is 6.4% lower, but this is **provisional performance evidence**.
The candidate reported Lua callback/frame deadline violations under the
fixture's Standard policy; the baseline did not. Optional draws were suppressed
on affected frames, so command output is not identical. Both runs also report
the same missing optional resource/sprite diagnostics for the synthetic chart.
The prepared index removes repeated structural work, but these measurements
do not establish a 6.4% in-game improvement or resolve the 1,500-versus-2,000 FPS
comparison. A live same-chart comparison remains necessary.

Compressed raw samples, diagnostics, model counts and executable identities
are in [the evidence directory](evidence/2026-10-09-windows-gameplay-frame-index).

## Windows compatibility and verification

The session test target needed standard C++ designated-initializer ordering
and a local variable rename to avoid the Windows `small` macro. Its malformed
CP932 fixture now recognizes Windows' existing replacement behavior; production
CP932 decoding remains permissive. The Pomyu path regression also exposed
locale-dependent filesystem conversions. Pomyu resource paths now explicitly
round-trip UTF-8, preserving names decoded from MS932 and multibyte characters
whose encoded trail byte is a backslash.

Regression coverage checks that live visibility changes still restore their
original draw output, that standalone mutable models retain duplicate-ID
validation after vector reallocation, and that the benchmark excludes warmup
frames. The existing Pomyu fixture covers the Windows Unicode path repair.

Final MSVC Release verification passed:

```powershell
cmake --build --preset release-windows --target main play_skin_session_tests gameplay_skin_loading_benchmark_tests skin_draw_command_tests --parallel 6
ctest --test-dir cmake-build-release-visual-studio -C Release --output-on-failure -j 6 -R '^(play_skin_session_tests|gameplay_skin_loading_benchmark_tests|skin_draw_command_tests)$'
```

All three focused suites passed, including the complete session suite. The
build used the existing build directory's configured CMake 3.30.5 executable
at `C:\Program Files\Git\mingw64\bin\cmake.exe`, with its adjacent CTest.
