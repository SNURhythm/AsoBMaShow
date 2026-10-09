# Reuse dropdown widths and manual text line breaks

Baseline: `abaefa1c`. Dependencies, font files and font-selection locking are
unchanged. This follow-up removes redundant work in two existing text consumers.

## Changes

- `DropdownView` retains its preferred width. Option changes (including indicator
  chrome), prefix changes and language revision changes invalidate it. Trigger
  allocation, menu width, selected option and enabled/open state still update
  normally. The existing option comparison remains; unchanged refreshes avoid
  resolving and shaping every option again. Language revision is checked even
  before explicit view propagation. Availability/color changes conservatively
  invalidate through the existing option comparison as well.
- `TextView` retains one set of manual line breaks for its current text and raster
  wrap width. Text replacement/clearing invalidates it; another width computes a
  new entry. Measurement and surface composition borrow the cached vector.
  Existing UTF-8 decoding, word breaks, fallback selection, CoreText fallback,
  alignment, descender handling and actual shaping/rasterization are preserved.
  Retained strings cover only the current layout, not a history of prior texts.

Native SDL3_ttf fallback was inspected but is not adopted: fallback lists and
wrap alignment mutate shared font state, and SDL fonts cannot replace the iOS
CoreText system-glyph source. Keeping the existing composer avoids changing
those contracts. This cache also serves explicit/multiline and aligned manual
composition; primary-font left-aligned native wrapping remains separate.

## Correctness

Regression tests wrap the actual `TTF_GetStringSize` function to count shaping
requests while forwarding every call to SDL_ttf. The old dropdown path failed
“unchanged dropdown labels are not reshaped during resize or refresh.” After
that fix, the old wrapper failed “unchanged line breaks do not reshape prefixes.”
Both pass with the two caches.

Tests separately cover fixed-trigger release, selection changes, indicator
addition/removal, prefix-only and option-only changes, empty options, language
changes before/after propagation, mixed icon/CJK fallback, narrower/wider widths,
text replacement, CRLF and clearing. Existing raster/descender/alignment tests
also pass. The mixed fixture uses `M` from Font Awesome and `あ` from fallback.

The desktop rebuild and **454/454 CTest entries passed**. Independent review and
the prefix-only coverage recheck found no actionable issue. No new mobile build,
physical-device run or Windows/Linux build was performed for this pass. Mobile
folding reaches the same width-key invalidation, but this is not a new Duo
simulator runtime check.

## Measurements

Final measurements use 20 balanced, shuffled before/after process pairs on an
Apple M1 Pro, with Release builds and the same dependencies. Compilation and
all tests finished before timing; process and batch warmups were excluded.
All retained samples and all metrics are recorded. The final 15 medians and
per-pair aggregate checksums were independently recomputed/checked.

- Four-option unchanged dropdown refresh: **88.1% less CPU time**, 2.819 → 0.334 µs;
  resizing: **45.8% less**, 5.422 → 2.939 µs.
- 16/128-option unchanged refresh/resize: approximately **96–99% less CPU time**.
  These larger lists exceed SDL_ttf's small layout cache and amplify repeated
  shaping costs. They are synthetic lists, not measured whole Settings screens.
- Repeated manual composition into a CPU surface: **64.9% less with the primary
  font**, 95.386 → 33.506 µs, and **19.0% less with mixed fonts**, 36.671 → 29.700 µs.
- Pure cached line-break access becomes a few nanoseconds. That helper-only
  result is not a full text update; composition still does measurement, glyph
  rasterization and blending.

Cache misses are not uniformly faster. The changed-label control with 16 options
rose **4.1% / 4.15 µs**, and mixed-font width changes rose **1.9% / 0.15 µs**.
Other miss controls were close to unchanged or slightly faster. The dropdown
changed-label case includes selected-trigger refresh/rasterization as well as
preferred-width measurement. The report retains these costs; it does not claim
that every text update improves or that any observed delta isolates one cause.

An initial complete 20-pair run used a value-returning helper that introduced an
extra candidate-only vector copy on wrapping cache misses. After observing its
+3.4% mixed resize delta, the harness was corrected to consume both versions via
a const reference inside the same scope, matching production. The corrected
run below still shows the smaller +1.9% miss cost. Both full runs and both harness
sources are retained; no production code changed between them and no samples
were filtered. This is a method correction, not selection of favorable samples.

Times are **µs/operation**; negative change means less CPU time. Intervals are
paired bootstrap estimates (20,000 resamples) within this session, exploratory
across 15 metrics. Aggregate checksums cover widths, line sizes and surface
bounds, not pixel equality. No GPU work, whole-frame rate, physical input latency
or mobile performance is measured. Manual primary-font results do not describe
SDL3_ttf's separate native left-aligned primary wrapping path.

| Metric | Before | After | Change | Paired 95% interval |
| --- | ---: | ---: | ---: | ---: |
| `dropdown_resize_4` | 5.4224 | 2.9394 | -45.79% | [-46.00%, -45.27%] |
| `dropdown_refresh_4` | 2.8188 | 0.3341 | -88.15% | [-88.23%, -87.99%] |
| `dropdown_changed_label_4` | 25.0777 | 25.3957 | +1.27% | [-1.06%, +3.19%] |
| `dropdown_resize_16` | 72.8046 | 2.9495 | -95.95% | [-96.00%, -95.83%] |
| `dropdown_refresh_16` | 70.4397 | 0.7383 | -98.95% | [-98.96%, -98.93%] |
| `dropdown_changed_label_16` | 101.8360 | 105.9835 | +4.07% | [+2.04%, +5.48%] |
| `dropdown_resize_128` | 566.5880 | 3.0120 | -99.47% | [-99.48%, -99.46%] |
| `dropdown_refresh_128` | 583.2580 | 4.6471 | -99.20% | [-99.23%, -99.16%] |
| `dropdown_changed_label_128` | 613.3620 | 600.6910 | -2.07% | [-3.75%, -1.07%] |
| `wrap_reused_primary` | 54.2043 | 0.0042 | -99.99% | [-99.99%, -99.99%] |
| `wrap_resized_primary` | 56.5532 | 55.4260 | -1.99% | [-3.29%, +0.34%] |
| `composed_raster_reused_primary` | 95.3859 | 33.5060 | -64.87% | [-65.33%, -63.20%] |
| `wrap_reused_mixed` | 7.0140 | 0.0042 | -99.94% | [-99.94%, -99.94%] |
| `wrap_resized_mixed` | 7.5590 | 7.7042 | +1.92% | [+0.70%, +5.04%] |
| `composed_raster_reused_mixed` | 36.6711 | 29.6998 | -19.01% | [-21.37%, -16.49%] |

[Reproduction and harness](evidence/2026-10-09-sdl3-text-layout/README.md),
[final raw data](evidence/2026-10-09-sdl3-text-layout/raw.json.gz),
[final summary](evidence/2026-10-09-sdl3-text-layout/summary.json),
[build/source fingerprints](evidence/2026-10-09-sdl3-text-layout/build.json),
and [initial value-returning probe results](evidence/2026-10-09-sdl3-text-layout/value-returning-probe/summary.json).
