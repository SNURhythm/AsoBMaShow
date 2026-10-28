# Gameplay repeated-work cleanup

**Goal:** Implement the four remaining findings from the built-in source
inspection while preserving rendering, captured-state lifetime and input behavior.
**Spec:** `docs/reviews/2026-10-28-built-in-renderer-source-inspection.md`.
**Architecture:** Camera owns inverse matrices updated alongside their sources;
batch rendering shares immutable corner samples; captured touch drawing consumes
resolved snapshots; gameplay snapshots share immutable option/course metadata.
**Tech stack:** C++23, SDL/bgfx, existing CTest and Metal characterization.

The user requested parallel implementation. Separate agents own camera,
metadata and shape sampling; the parent owns touch rendering and test/build
integration. Builds are serialized within each build directory. No presentation
scheduling, drawable count, parser, deployment or public setting changes.

- [x] Cache camera inverses at existing matrix-write boundaries. Verify all
  setters, edit/commit, render-before-deproject and viewport/depth conventions.
- [x] Reuse rounded-corner unit samples. Compare all segment-count boundaries,
  radius clamping and rectangle fallback against original tessellation pixels.
- [x] Eliminate captured touch event-map reconstruction. Reproduce allocation
  growth and missing held Move markers before fixing; verify hide/show and fade.
- [x] Share option/course metadata by effective option/seed/title content.
  Verify allocation reuse, invalidation, retained frame lifetime and legacy
  preview/export getter fallback.
- [x] Complete eight focused suites, desktop and unsigned iOS builds, independent
  review and final diff verification.

Review focus: camera read concurrency; immutable snapshot ownership; replay
rewind and released touch lifecycle; multiple pointers with reused finger IDs;
course/practice transitions; exact shader-input geometry at segment boundaries.
