# Text decoding cache experiment

The first change on `perf/text-operations` reuses decoded Unicode scalars in
`Skin2DRenderer`. It adds no dependency and does not change Lua evaluation,
font normalization, or layout rules.

## Scope

- A renderer owns 256 cache slots, each accepting at most 1024 UTF-8 bytes.
- A hash selects a slot; a full-string comparison decides a hit. Collisions
  replace that slot and can reduce reuse, but cannot select another string's
  codepoints.
- Stored text and reserved codepoint payload are bounded to approximately
  1.25 MiB, plus string capacity and object overhead. Prepared layouts can
  retain evicted vectors until their frame finishes.
- Decoded storage is shared across unchanged runs. Changing values reuse the
  slot's vector only when no prepared layout still holds it.
- Atlas membership and frame glyph budgets are checked on every use. CR and
  unavailable glyph filtering allocate a separate vector only when needed.
- Oversized and malformed strings use the original scalar path, preserving
  early glyph-limit failures and diagnostic precedence. Session changes clear
  the cache.

## Measurement

An opt-in CPU benchmark lives in `skin_draw_command_tests`:

```sh
cmake --build cmake-build-debug --target skin_draw_command_tests -j 6
cmake-build-debug/skin_draw_command_tests --benchmark-text
```

Use matching optimized builds for performance comparisons; the command above
also works in Debug but its timings are not comparable to the numbers below.
The benchmark evaluates 32 text objects with ASCII, Korean, and emoji,
producing 2,448 glyphs per frame. Each case warms up for 100 frames and measures
2,000 frames. The changing case generates a different value for every object
on every measured frame. There is no GPU submission, Lua callback, or file I/O
in the timed loop.

Local result: Apple M1 Pro, macOS 26.5.1, Apple Clang 21.0.0. Project objects
for the test target were compiled with `-O3 -g0` using the compilation database
and linked against the same libraries from the existing Debug build. Seven
alternating baseline/candidate runs were measured after other builds finished.
The baseline renderer is commit `3ecad03695e0ccbef5acc8bd566bbc65421e1cc6`, with
the same benchmark added.

| Case | Baseline median | Cache median | Change |
| --- | ---: | ---: | ---: |
| Unchanged text | 90.89 µs/frame | 83.89 µs/frame | 7.7% less CPU time |
| Every value changes | 93.31 µs/frame | 92.15 µs/frame | Within observed timing variation |

Both versions emitted 4,896,000 glyphs per measured case. The result supports
keeping this small decoding optimization, but does not establish an FPS
improvement or predict mobile performance. Layout, kerning, glyph lookups,
and command generation still execute each frame. Profile those costs before
expanding to layout caching or adding simdutf.

## Regression coverage

Cache tests cover mixed Unicode, embedded NUL, malformed encodings, full-string
identity under forced collisions, bounded entries, retained layouts during
replacement and clearing, failed decoding into reused storage, and moves.
Renderer tests cover repeated values, CRLF layout, changing text, changed
atlas contents, bitmap glyph filtering, malformed-value diagnostics, and a
warmed short string exhausting the remaining frame glyph budget. Existing
command fixtures continue to check rendered geometry and ordering.

Validation: the desktop `main` target and renderer/session test targets built
successfully; the full parallel CTest run passed all 401 tests. The standalone
cache tests also passed AddressSanitizer and UndefinedBehaviorSanitizer.
