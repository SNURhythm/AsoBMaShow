# Full-suite runtime optimization — 2026-10-17

The target is a full desktop CTest run under 60 seconds with six test slots,
without relying on a populated compiler cache. This measures test execution,
including the fixture compilation performed by Python tests, after building all
native test targets. It does not measure a clean application/test build.

## Changes and retained coverage

- The native download bridge uses an extracted retry-wait helper. Production
  still uses the monotonic clock, 1/2/4-second delays and 100 ms cancellation
  polling. The HTTP integration fixture supplies a controlled clock and sleeper
  to that same helper, checking the exact accumulated delay, retry counts,
  cancellation, completed bytes and recovery prompts. HTTP requests and native
  file operations remain real. This removes approximately 27 seconds of waiting.
- Loopback transports, profile fault injection and deterministic Lua semantics
  no longer serialize the entire suite. Ports, temporary files and fault hooks
  are isolated. The host-module suite now uses the existing controlled-clock
  hook. The Lua runtime's actual deadline checks remain serialized.
- The chart repository fixture uses 2,048 rows instead of 50,000, still exceeding
  its 1,000-step full-scan guard. The PNG fixture remains four times larger than
  the permitted allocation ceiling; the optional 16,384-pixel stress mode stays.
- Ordinary overlapping Ogg fixtures span three decode blocks, exceeding the
  two-opens-per-source guard. Repeated voices still expire between bursts, and
  the 192-voice descriptor-limit fixture retains its original audio length.
  Long seeks still exceed both the 10-second read-ahead and 20-second eager-work
  guard, with exact reference PCM comparisons.
- Test-created 7z archives use compression level 1. The 513 MiB archive threshold
  case and its full-byte verification remain. Metal readback buffers use bulk
  initialization and retain the existing pixel checks.
- Native tests follow the application's existing `ENABLE_DEBUG_OPTIMIZED_BUILD`
  policy (`-O1` and frame pointers in Debug on non-MSVC compilers). Assertions
  remain enabled. This also optimizes fixture translation units, which otherwise
  instantiate slow STL/JSON routines even when production sources are optimized.
  Application flags and Release settings are unchanged; the existing option can
  still disable Debug optimization.
- Generated UI and music-selection fixtures share standard-header parsing and
  stable dependency-object compilation within one Python invocation. Artifacts
  are created afresh on each invocation and deleted at exit. Executions retain
  separate processes and working directories. MSVC and non-Clang compilers keep
  their ordinary compilation path. No prebuilt application localization library
  is reused: fixture compiler, ABI and instrumentation settings remain independent.

## Measurements

The starting existing-checkout run passed 498/498 tests in 116.64 seconds with
the normal compiler cache. A subsequent all-target rebuild refreshed stale
objects, so this is context rather than a controlled cold-cache before/after
comparison.

An intermediate version with only selected source files optimized passed
498/498 tests in 91.61 seconds with `CCACHE_DISABLE=1`. This missed the target
and motivated applying the existing Debug policy to complete test targets.

The complete target rebuild succeeded. Final full-suite measurements on the
local macOS/arm64 Debug build, all using `ctest --test-dir cmake-build-debug
--output-on-failure -j 6`:

| Run | Compiler cache | Result | Elapsed |
| --- | --- | --- | ---: |
| First after the complete target rebuild | Disabled (`CCACHE_DISABLE=1`) | 498/498 passed | 74.24 s |
| Repeat, new fixture processes/artifacts | Disabled (`CCACHE_DISABLE=1`) | 498/498 passed | 55.13 s |
| Explicit cold-cache check | New empty `CCACHE_DIR` | 498/498 passed | 58.34 s |

The empty cache recorded 66 misses, 13 hits generated within that same run,
and 65 uncacheable calls. It was deleted after measurement. The disabled-cache
run demonstrates that the target does not require those within-run cache hits.

The initial post-rebuild run remained above 60 seconds. Several tiny executables
took approximately 0.2 seconds there but approximately zero when checked again;
the cause of that launch/scheduling overhead was not isolated. These results meet
the target with cold or disabled **compiler** caching, but do not establish a
sub-60-second guarantee immediately after rebuilding every native executable or
with cold operating-system caches. No operating-system caches were cleared.

No CTest entries were removed. Static review found and resolved a potential
ABI/sanitizer mismatch in an intermediate localization-library reuse approach;
the final version builds those objects with the fixture's own compiler contract.
The full suite covers the changed helpers, GNU/MSVC/clang-cl command checks and
shared-source partitioning. `git diff --check` also passed. Platform builds and
deployments were not part of this desktop runtime measurement.

To repeat the compiler-cache-independent check after building all native targets:

```sh
cmake --build cmake-build-debug -j 6
CCACHE_DISABLE=1 ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

## Follow-up toward 30 seconds

Enabled the existing per-invocation header sharing in preview lifecycle, menu
tabs, replay export cancellation and settings initialization fixtures. This
changes 11 call sites without new infrastructure, persistent fixture artifacts,
or reduced assertions. The isolated preview suite improved from 6.18 to 3.74
seconds with compiler caching disabled. The other three affected suites passed
together in 2.82 seconds after the change.

The full cache-disabled `-j 6` run passed 498/498 tests in **54.92 seconds**.
That is effectively unchanged from the earlier 55.13-second measurement;
focused savings do not establish a significant whole-suite improvement.
Profiling the music-selection fixture suite found roughly 15 seconds spent on
compilation/linking and 10 seconds on subprocess execution. The four real
feature-disabled translation-unit checks and real-time audio checks also remain
substantial costs. Stopped this pass at the small, verified improvements:
30 seconds was not reached, and further large gains appear to need broader
fixture consolidation or scheduling work.
