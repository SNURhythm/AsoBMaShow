# Text font-selection cache: avoid redundant runtime locking

Application baseline: `3e09cbdc`. SDL3, SDL3_ttf, font files, HarfBuzz,
compiler settings and the existing benchmark harness are unchanged.

## Change and ownership

`TextView::selectFont` previously acquired the process-wide recursive SDL_ttf
operation mutex before checking its own font-selection cache. Width measurement
and fallback composition call it for every codepoint, including warm cache hits.
The cache lookup now precedes that guard. A hit only copies a pointer/selection;
glyph inspection, loading, shaping and rasterization retain their guards.

Views and their fonts already belong to their creating thread. A live view holds
both a runtime reference and references to its loaded fonts, preventing final
`TTF_Quit` and active-font eviction. The old selection guard ended on return and
did not protect later pointer use. This change does not permit concurrent access
to one view or change font choice, normalization, layout or rendered glyphs.

## Measurement

On the Apple M1 Pro, 20 balanced, shuffled before/after process pairs showed:

- Four repeated Latin strings: width CPU time **49.4% lower**, 1.024 → 0.518 µs.
- Four repeated CJK strings: width CPU time **43.3% lower**, 0.813 → 0.461 µs.
- 128 varied strings: width CPU time **12.6% lower for Latin**, **3.2% lower for CJK**.
- Deferred update time was essentially unchanged. The earlier native whole-string
  measurement optimization already bypasses font-run selection in this case.

These are warm synthetic workloads, not aggregate frame time or mobile results.
The 128 strings use four base phrases plus numeric suffixes; they are not 128
independent chart titles. Raster metrics include `ensureFontsForText` and cached
selection before surface rendering, so their differences do not isolate SDL_ttf's
rasterizer. The unchanged event-queue control shifted +1.5% (less than 1 ns);
small changes should not be interpreted as caused by this text edit.

All process and batch warmups were excluded, all retained samples were kept,
and no builds or tests overlapped timing. The test app on the Duo simulator was
terminated before timing. Intervals are paired bootstrap estimates over 20,000
resamples within this one session; there are 15 exploratory metrics. Aggregate
width/surface-size checksums match in every pair, but are not pixel hashes.

Times are **µs/operation**; negative change means less CPU time.

| Metric | Before | After | Change | Paired 95% interval |
| --- | ---: | ---: | ---: | ---: |
| `event_queue_64` | 0.0525 | 0.0533 | +1.5% | [+0.6%, +2.8%] |
| `touch_watch_ingress` | 0.0943 | 0.0954 | +1.1% | [-1.0%, +2.2%] |
| `text_width_latin_repeated4` | 1.0238 | 0.5181 | -49.4% | [-50.8%, -48.3%] |
| `text_update_latin_repeated4` | 0.9555 | 0.9426 | -1.3% | [-3.4%, +1.5%] |
| `text_raster_latin_repeated4` | 7.1367 | 6.6489 | -6.8% | [-9.7%, -4.9%] |
| `text_width_cjk_repeated4` | 0.8129 | 0.4611 | -43.3% | [-44.5%, -42.0%] |
| `text_update_cjk_repeated4` | 0.7861 | 0.7791 | -0.9% | [-2.7%, +1.0%] |
| `text_raster_cjk_repeated4` | 7.1700 | 6.6325 | -7.5% | [-11.7%, -4.8%] |
| `text_width_latin_diverse128` | 4.5109 | 3.9438 | -12.6% | [-14.3%, -11.1%] |
| `text_update_latin_diverse128` | 4.5965 | 4.5519 | -1.0% | [-2.1%, +1.4%] |
| `text_raster_latin_diverse128` | 11.8087 | 11.0118 | -6.7% | [-10.0%, -3.7%] |
| `text_width_cjk_diverse128` | 16.4666 | 15.9320 | -3.2% | [-4.4%, -2.0%] |
| `text_update_cjk_diverse128` | 16.1710 | 16.1569 | -0.1% | [-1.4%, +2.2%] |
| `text_raster_cjk_diverse128` | 24.1083 | 22.9715 | -4.7% | [-8.2%, +0.6%] |
| `cached_text_view_create` | 1.4887 | 1.4741 | -1.0% | [-3.3%, +0.5%] |

## Verification and limits

- Desktop `main` and four affected test targets built successfully.
- Four focused CTest entries passed: font lifetime, text input, chart-list labels,
  and Settings button layout. These cover fallback alignment, descenders, wrapped
  metric/raster agreement, constructor rollback, cache retention/eviction and
  creating-thread font separation.
- Independent source review found no actionable correctness issue. It confirmed
  active font/runtime references and guarded downstream SDL_ttf calls. Existing
  tests do not stress concurrent warmed selection against other-thread teardown.
- No new full-suite, mobile build, physical-device, sanitizer or Windows/Linux
  validation was run for this narrow lock-scope change. Prior migration results
  are separate evidence, not new validation of this revision.

## Remaining text opportunities

`DropdownView::preferredWidth` remeasures every option when widths refresh;
cache invalidation must include resolved language, labels and indicator chrome.
Manual fallback wrapping also repeatedly shapes growing prefixes. Native SDL3_ttf
fallback/wrapping merits a separate design: fallback lists and wrap alignment are
properties of shared fonts, while iOS still needs CoreText for system-only glyphs.
Neither larger change is included here.

[Raw paired results](evidence/2026-10-09-sdl3-text-cache/raw.json.gz),
[summary](evidence/2026-10-09-sdl3-text-cache/summary.json),
[build/source fingerprints](evidence/2026-10-09-sdl3-text-cache/build.json),
[measured production patch](evidence/2026-10-09-sdl3-text-cache/measured-change.patch.gz),
and [reproduction](evidence/2026-10-09-sdl3-text-cache/README.md).
