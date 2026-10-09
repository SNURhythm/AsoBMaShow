# Adapting application code to SDL3

## Changes

Reviewed the application after the initial migration and measured the resulting
changes against `53f52b9e` on 2026-10-09.

- **Native text measurement.** Primary-font labels now use one whole-string
  measurement. Primary-font wrapped labels use `TTF_GetStringSizeWrapped`, which
  shares its wrapping implementation with SDL3_ttf's actual surface renderer.
  This avoids the old fallback-run construction and repeated shaping of growing
  prefixes. A regression test demonstrated the old discrepancy: at logical
  width 83, “AVATAR office gjpqy” measured 105 pixels high but rendered 140 pixels
  high. The new measurement matches the raster. Mixed-font, explicit multiline,
  and center/right wrapped composition retain their existing paths.
- **Event-local pointer targeting.** ScrollView, RecyclerView, ranking viewport,
  and chart viewer use `wheel.mouse_x/mouse_y`; recycler clicks use button `x/y`.
  Polling `SDL_GetMouseState` after pumping events could target the pointer's
  later position instead of the position where the event occurred. Coordinate
  transforms now retain floats; wheel direction and fractional deltas are kept.
- **Metal view ownership.** The native-window helper transfers the owning
  `SDL_MetalView` into the existing RAII resource wrapper. Main releases it after
  bgfx shutdown and before destroying the SDL window. Failed layer extraction
  releases the partially created view. This fixes a retained view/layer; it is
  not a claimed frame-time optimization.

The review also checked SDL device enumeration, timestamps, display sizing,
event ownership, and IO. Those paths already use SDL3 conventions. Audio is
miniaudio and rendering is bgfx. A replacement audio backend, SDL_GPU renderer,
or SDL_ttf text-engine integration is a separate design change without a
measured need in this task. HarfBuzz shaping remains enabled.

## Recursive dependency verification

Ran `git submodule sync --recursive -- SDL SDL_ttf` and
`git submodule update --init --recursive -- SDL SDL_ttf`. All nested checkouts
match the commits recorded by the selected forks. The dependency sources were
already populated during the earlier benchmark; the parent repository's local
submodule registration was refreshed. No dependency version pins changed.
This verifies the pinned dependency graph, not an upgrade to arbitrary upstream
branch heads.

| Component | Pinned commit |
|---|---|
| SDL | `3d22d98cf633b673aae4f278ea55775481ae9279` |
| SDL_ttf | `661ba00eb142c060768b752c9420b50c961b909e` |
| FreeType | `535d2993d2cab67cc8b270132b1b1bcb62f269aa` |
| FreeType / dlg | `72dfcc858c040c54a6a0b88fcb7e70ee186d3167` |
| HarfBuzz | `950d232cdcd29e2e83a2099307e5624a8f1aa937` |
| plutosvg | `96e115bc0f837dbccf260a05ad23978b6b54f731` |
| plutosvg / plutovg | `bbd91f0d06a71491691b36330f29dffa4af87ccf` |
| plutovg | `a6e0d682d08a5e993694ad82a23e5d45e831f315` |

SDL has no nested Git submodules. Both nested and direct plutovg checkouts are
intentional entries in the pinned dependency graph.

## Performance

The comparison holds **SDL3, SDL3_ttf, FreeType, HarfBuzz ON, font, compiler and
Release optimization settings constant**. It isolates the application change,
unlike the previous [SDL2-to-SDL3 comparison](2026-10-09-sdl3-performance.md),
where the font dependency configuration also changed.

On an Apple M1 Pro, 20 balanced before/after pairs (40 retained process runs)
showed these reductions in CPU time for deferred text updates:

- 128-string Latin workload: **40.6%**, 7.44 → 4.42 µs.
- 128-string CJK workload: **14.2%**, 18.14 → 15.56 µs.
- Four repeated Latin strings: **73.3%**, 3.44 → 0.92 µs.
- Four repeated CJK strings: **73.9%**, 2.91 → 0.76 µs.

The four-string workload fits SDL3_ttf's eight-entry layout-position cache.
The 128 strings are four base phrases with numeric suffixes, not a sample of
128 independent chart titles. Glyph/file caches are warm. “Update” excludes GPU
texture materialization. The benchmark does not time wrapped labels, so the
new wrapped API's performance benefit is not quantified here.

Standalone Latin width measurement remains slower than the earlier SDL2
baseline. This update fixes redundant application work, not the entire effect
of switching font stacks. There is no additional material rasterization gain;
small changes also occur in unchanged paths, including +2–4% in repeated width
measurements (about 18–40 ns per operation). All results are retained below.
No overall FPS, physical input latency or mobile speedup was measured.

Times are **µs per operation**, lower is better. Percentage is
`(after median / before median − 1) × 100`. Intervals are paired percentile
bootstrap intervals over 20,000 resamples, reflecting variability in this one
session, not all devices/workloads; they are exploratory across 15 metrics.
The process and per-metric batch warmups are excluded. No retained outliers
were removed, and no compilation overlapped the retained measurement runs.

| Workload | Before | After | Time change | Paired 95% interval |
|---|---:|---:|---:|---:|
| `event_queue_64` | 0.0515 | 0.0511 | -0.7% | [-2.1%, +0.5%] |
| `touch_watch_ingress` | 0.0913 | 0.0913 | +0.0% | [-1.5%, +0.8%] |
| `text_width_latin_repeated4` | 0.9602 | 0.9997 | +4.1% | [+3.5%, +5.6%] |
| `text_update_latin_repeated4` | 3.4347 | 0.9174 | -73.3% | [-73.6%, -72.8%] |
| `text_raster_latin_repeated4` | 6.9819 | 7.0184 | +0.5% | [-0.5%, +1.7%] |
| `text_width_cjk_repeated4` | 0.7710 | 0.7883 | +2.3% | [+1.9%, +3.2%] |
| `text_update_cjk_repeated4` | 2.9130 | 0.7610 | -73.9% | [-74.0%, -73.7%] |
| `text_raster_cjk_repeated4` | 6.8486 | 6.8456 | -0.0% | [-1.3%, +2.2%] |
| `text_width_latin_diverse128` | 4.2816 | 4.3556 | +1.7% | [+1.2%, +2.2%] |
| `text_update_latin_diverse128` | 7.4362 | 4.4180 | -40.6% | [-41.2%, -39.8%] |
| `text_raster_latin_diverse128` | 11.0984 | 11.1500 | +0.5% | [+0.2%, +1.0%] |
| `text_width_cjk_diverse128` | 15.9389 | 15.5869 | -2.2% | [-4.4%, +1.4%] |
| `text_update_cjk_diverse128` | 18.1417 | 15.5576 | -14.2% | [-15.3%, -13.6%] |
| `text_raster_cjk_diverse128` | 22.4120 | 22.4043 | -0.0% | [-0.9%, +0.5%] |
| `cached_text_view_create` | 1.4243 | 1.4331 | +0.6% | [-0.1%, +1.2%] |

The before executable matches the SHA-256 fingerprint of the previously
reviewed SDL3 benchmark. Its sampled application sources are unchanged between
`ef61b16f` and `53f52b9e`. Both sides use the same original harness, and the
candidate TextView source fingerprint is recorded. All 15 changes were
independently recomputed from the raw process medians. Aggregate text-width and
surface-size checksums match between versions in every pair; these checksums
are not pixel hashes or proof of complete UI output identity.

## Verification

- New text geometry test failed against the old manual measurement, then passed
  against native SDL3 measurement. It compares real surface bounds across three
  widths, wrapped/unwrapped state, Latin, whitespace, long words, Japanese and
  Korean, plus clearing text. Existing fallback, descender and alignment tests
  also pass.
- New ScrollView and RecyclerView event-position tests failed on global pointer
  polling, then passed with event coordinates. Existing natural/fractional
  scrolling and touch cancellation tests pass.
- Metal ownership unit test covers explicit reset, scope cleanup, create failure
  and failed layer extraction. A separate native macOS Metal smoke completed
  three create/frame/resize/shutdown/view-release/window-release cycles.
- Independent code review found no blocking issues in the implemented changes.
- Full desktop build and **453/453 CTest tests passed**.
- Unsigned iOS device build and packaged-artifact audit passed using
  `scripts/ios_firebase_deploy.sh --build-only` and `scripts/ios_artifact_audit.sh`.
- Signed Android `restricted_file_accessRelease` build passed using
  `scripts/android_firebase_deploy.sh --build-only`.

Native macOS smoke and synthetic tests do not establish iOS hardware lifecycle
behavior, real multi-DPI pointer routing, or performance on Windows/Linux/mobile.

[Raw paired runs](evidence/2026-10-09-sdl3-adaptation/raw.json),
[computed results](evidence/2026-10-09-sdl3-adaptation/summary.json), and
[reproduction instructions](evidence/2026-10-09-sdl3-adaptation/README.md).
