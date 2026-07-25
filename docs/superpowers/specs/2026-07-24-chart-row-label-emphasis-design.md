# Chart Row Label Emphasis Design

## Goal

Make the chart-row difficulty and key-mode labels easier to scan over banner artwork by rendering both roles in bold, without changing their size, color, alignment, or content.

## Design

Add a semantic `TextView::FontWeight` enum with `Regular` and `Bold` values. The `TextView` constructor accepts an optional weight that defaults to `Regular`, preserving every existing call site. `fontWeight()` exposes the configured value for focused UI tests.

SDL_ttf provides synthetic bold through `TTF_STYLE_BOLD`. Because this repository shares loaded fonts through a cache, the cache key must include the SDL_ttf style. Regular and bold views therefore use separate `TTF_Font` instances, preventing a bold label from mutating the appearance of unrelated regular text. Every acquired fallback face uses the same weight and is released through the matching style-aware cache key.

`ChartListItemView` constructs `levelView` and `keyModeView` with `FontWeight::Bold`. The weight applies to those roles in normal, course, archive, and missing rows. Title, artist, score-rank, and favorite UI retain their current typography.

No new font asset is added. No platform-specific rendering path or Windows shader work is introduced.

## Testing and verification

- Name the difficulty and key-mode views so the existing headless chart-row test can find them.
- First assert that both views report `FontWeight::Bold`; observe the test fail before the weight API and row configuration exist.
- Preserve all existing chart-banner fade, scrim, layout, eligibility, and theme assertions.
- Build the complete project, run all 140 CTest tests, and explicitly build `main` before pushing the existing branch.

