# Chart Source Syntax Implementation Plan

> **For agentic workers:** Use focused native platform tasks with disjoint files; the parent integrates and verifies.

**Goal:** Fast syntax colors in the native source viewer without touching chart parsing.

**Architecture:** A standalone scanner produces compact immutable UTF-16 spans.
Native backends retain only a bounded viewport's foreground attributes and reuse
the cached spans. Existing selection, practice backgrounds and lifecycle stay intact.

**Tech Stack:** C++23, UIKit/AppKit, Android TextView/JNI, Win32 Rich Edit.

**Spec:** `docs/superpowers/specs/2026-10-24-source-syntax-design.md`

## Constraints and review focus

- No parser repo, parser amalgamation, Parse or Scan modifications.
- Tokenize only on source open, never on scroll, resize or theme change.
- One span for channel data regardless of cell count; no regex or token strings.
- Bound active styling to 16,384 UTF-16 units and 4,096 spans; preserve native selection and practice backgrounds.
- Check Unicode, CRLF, literal filenames/comments, long wrapped lines and distant jumps after scrolling.
- No worktrees, deployments or concurrent builds in one build directory.

## Tasks

- [x] Add `src/view/BmsSyntax.h/.cpp`: compact `Kind`, `Span`, immutable `Document`, `tokenize`, and bounded `paintWindow` lookup. Test lexical categories, UTF-16 offsets, long payloads, dense lines and lookup bounds in `tests/bms_syntax_tests.cpp`; add an opt-in benchmark mode.
- [x] Extend `NativeDocument.h` with palette colors and `Surface::setSyntax(shared_ptr<const bms_syntax::Document>)`. Wire source open/theme palette in `SelectableDocumentView.cpp`; register focused tests and source files in existing CMake files.
- [x] Apple: use cached spans and viewport callbacks to update bounded foreground attributes. Extend the actual AppKit test for colors, scroll bounds, selection/background preservation and theme changes; compile UIKit device/simulator.
- [x] Android: transfer compact spans once via JNI, binary-search them on viewport updates, recycle only bounded foreground spans. Keep UI lifecycle/cancellation and native selection unchanged; compile JNI and Java.
- [x] Windows: map CRLF offsets once, apply bounded TOM foreground ranges with selection preserved, observe scroll/resize, and clear retired colors. Extend the Windows integration test; report unavailable Windows validation honestly.
- [x] Measure large tokenizer inputs and native styling bounds; run focused tests and desktop build, review integration, commit/push and update PR #154.
