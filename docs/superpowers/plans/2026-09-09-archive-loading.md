# Archive Loading Implementation Plan

> **For agentic workers:** Use parallel, disjoint implementation tasks with test-first regressions and a final integrated review.

**Goal:** Implement the five approved archive-loading improvements without changing playback readiness or audio ownership.

**Architecture:** Resolve one chart resource plan, retain unchanged decoded sounds, and stream missing audio plus all referenced visuals through a bounded consumer pipeline. Audio publication and archive cancellation are independent changes.

**Tech Stack:** C++23, existing archive backends, AudioWrapper, bgfx, CMake/CTest.

**Spec:** The approved five-point archive-loading review in this conversation.

## Global Constraints

- Work in the current checkout; no worktrees, branches, commits, pushes, or deployments.
- Preserve unrelated changes and surrounding formatting; do not edit amalgamated parser files.
- All referenced visuals, including layers and poor-BGA resources, remain ready before playback.
- Preserve authoritative audio ownership, lifecycle failure propagation, and archive integrity checking.
- Cache reuse requires unchanged archive identity; cancellation must not restart fallback work.
- Serialize shared CMake builds through the parent agent.

## Task 1: Unified Resource Loading (Parent)

**Files:** `src/audio/Jukebox.cpp`, `src/audio/Jukebox.h`, a narrowly scoped bounded pipeline header, `tests/jukebox_restore_tests.cpp`.

- [x] Add regressions for retained shared sound handles, retired removed/replaced sounds, combined audio/visual extraction, and ready layer/video resources.
- [x] Run the Jukebox test target and observe the missing reuse/combined behavior.
- [x] Replace the special destructive archive path with unified differential resource reconciliation and archive-state invalidation.
- [x] Add byte-bounded extraction/decode overlap, retaining buffer accounting through consumption, cancellation, and consumer failures; cover these with deterministic tests.
- [x] Verify initial load, sibling switches, resource reload, and visual-only loading, including corrupt/missing assets and cancellation.

## Task 2: Resampling Publication (Halley)

**Files:** `src/audio/AudioWrapper.cpp`, `src/audio/AudioWrapper.h`, existing audio backend tests.

- [x] Add concurrency/lifecycle regressions and observe their failure before implementation.
- [x] Resample outside the sound map lock, validating rate/configuration before publication and handling cancellation and duplicate loads.
- [x] Run focused audio tests and report evidence.

## Task 3: Archive Cancellation (Bohr)

**Files:** `src/ArchiveFile.cpp`, `src/ArchiveFile.h`, `tests/archive_file_concurrency_tests.cpp`.

- [x] Add deterministic large-entry and shared-handle cancellation regressions.
- [x] Make ZIP read/inflate/CRC loops and 7-Zip handle acquisition cancellable, preserving serialization and integrity validation.
- [x] Make cancellation terminal across fallbacks and run focused archive tests.

## Integration

- [x] Review disjoint patches together for lock ordering, buffer ownership, failure cleanup, and readiness.
- [x] Build desktop `main`, run relevant native tests, then full parallel CTest.
- [x] Re-run the synthetic sibling-switch probe and report measured results without extrapolating to real libraries.
- [x] Obtain an independent review and resolve actionable findings.
