# Gameplay allocation cleanup implementation plan

**Goal:** Remove the six source-confirmed rendering allocation patterns without
changing pixels, timing, note ordering or touch behavior.

**Architecture:** Retain scene-owned batch storage and controller geometry;
invalidate geometry by configuration, key mode and canvas. Guard lane-order
requests by presentation revision. Split scalar Hi-Speed math from traversal
payload construction. Normalize indicators only when content changes and omit
hidden lists from captured state. Recycle long-note map nodes rather than
replacing hashing with a potentially quadratic scan.

**Tech stack:** C++23, SDL/bgfx, existing native CTest fixtures.
**Spec:** `docs/reviews/2026-10-28-built-in-renderer-source-inspection.md`.

Constraints: current checkout only; preserve iOS 15 support, draw order,
multiple long notes per lane, orphan/dead HCN behavior and render budgets.
No parser edits, new metrics or deployment. Independent agent performs
read-only inspection for additional findings.

- [x] Extend built-in characterization with allocation checks for repeated
  scalar Hi-Speed and unchanged indicator updates; observe failure first.
- [x] Retain overlay/course ring batches in GamePlayScene; add a reusable
  VirtualControllerLayoutCache keyed by every geometry input and exercise
  unchanged reuse, resize, mode, configuration and disable/re-enable.
- [x] Cache the last applied built-in touch revision, invalidate when the
  input handler is replaced or presentation leaves built-in mode.
- [x] Extract configured Hi-Speed math, avoid hidden indicator copies and
  normalize only on content changes. Recycle unordered-map node handles for
  long-note scratch, preserving lookup and iteration semantics.
- [x] Run existing renderer equivalence/golden, projection, presentation and
  touch-router coverage plus desktop main and unsigned iOS compilation.
- [x] Review independent findings and record verified limits.

Review focus: controller resizing/rotation and selected deck; toggling hidden
indicators; input-handler replacement; multiple/orphan/HCN long notes; unchanged
Hi-Speed values across label units. Existing renderer equivalence coverage
checks the legacy long-note path against captured rendering.
