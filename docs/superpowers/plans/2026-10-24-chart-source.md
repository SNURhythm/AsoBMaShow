# Chart source viewer implementation plan

Goal: open decoded chart source in a full-page overlay and highlight/focus source data for the current practice range.

Architecture: expose the existing parser decoder in its upstream repository, use the viewer's retained bytes, map BMS data cells to parsed timeline times, and extend the virtualized document reader with byte-range highlights. Keep the overlay separate from chart navigation and preserve the practice selection when closing it.

- [x] Expose `Parser::DecodeText` without changing detection behavior; test legacy encodings and line endings upstream, then run the required parser/amalgamation suites.
- [x] Add a source document model and focused tests for partial measures, BPM/STOP timing, measure scales, random branches, CRLF and empty matches.
- [x] Extend `DocumentTextReader` with wrapped highlight regions and focus; test resize, recycled rows and focus in the existing UI suite.
- [x] Add localized View source / Highlight selection controls, full-page overlay, safe-area layout and isolated keyboard/gamepad navigation.
- [x] Run focused native/UI tests and the desktop main build; review changes, commit and push both repositories.

Constraints: no worktrees, no whole-file formatting, no deployment; parser edits originate in `../bms-parser-cpp`. Use existing chart timing and encoding behavior, including source order and active random values. Source text remains read-only and virtualized.

Verification: upstream parser `make clean && make test && make test_amalgamation`
passed (parser commit `140bd01`). The desktop `main` target builds. Focused
`chart_source_tests`, `text_input_box_tests`, `chart_viewer_lifecycle_tests`,
`localization_catalog_tests`, and `scene_presentation_activity_tests` all pass.
The app's parser files match the generated upstream amalgamation byte for byte.
