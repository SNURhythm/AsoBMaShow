# Music-select folder status implementation plan

> **For agentic workers:** Execute the regression/fix/verification steps in order.

**Goal:** Restore type-5 folder numbers, lamps, and distribution graphs using the pinned local Beatoraja source, not skin-specific expectations.

**Architecture:** Keep root and child descriptors lazy. Load status independently for the displayed directory rows on a cancellable background worker and install results by stable bar identity. Share the aggregation with the eager projection and query only the relevant directory, search, or table level.

**Tech Stack:** C++23, SQLite, existing CMake/CTest tests.

**Spec:** `docs/superpowers/specs/2026-09-01-beatoraja-lua-music-select-design.md`.

## Source findings

- Authority remains local Beatoraja `c2ed5db1a46145ed10790c3872f717e95b59db9d`.
- `BarManager.BarContentsLoader` calls `updateFolderStatus()` for displayed directories, independently of opening them.
- `FolderBar` queries song `parent`, not `folder`. `SQLiteSongDatabaseAccessor` stores `parent` from the chart path's grandparent. This is neither direct-file nor recursive-descendant aggregation.
- `DirectoryBar` counts installed records before SongBar deduplication; filters mode, not difficulty/invisibility; clears old arrays before replacing them; uses score notes for the 28 rank buckets.
- Hash, search, and command bars implement status. Table, container, and same-folder bars inherit a no-op.
- Integer properties 300 and 320–330 and both standalone/nested distribution graphs consume the same selected/displayed bar arrays.
- The reverted full-record root query missed the path semantics and the independent status lifecycle. Do not restore that scan on the UI thread.

## Steps

- [x] Add repository query regressions for immediate child song folders, archive paths, missing folder metadata, and duplicate chart sources; fix the scoped query without changing native exact-folder queries.
- [x] Add projection regressions for parent-folder membership, replacement aggregation, missing charts, mode/LN changes, and no-op directory subclasses; fix the common projection.
- [x] Add worker and bar-manager regressions proving unopened displayed rows receive status, stale results cannot overwrite a newer request, and navigation/children are preserved. Wire the same worker into the scene with immutable score/filter snapshots and teardown.
- [x] Verify selected-directory integer/Lua properties and standalone/song-list graphs against upstream; fix mismatches found in this path.
- [x] Update compatibility evidence; run focused tests, desktop `main`, and parallel full CTest. No deploy, commits, worktrees, or whole-file formatting.

## Review follow-ups

- Native Play Options LN changes now refresh song scores/replays and directory
  status, and the worker deduplication key includes LN mode.
- Command children retain visibility/feature metadata and source SongBar
  deduplication, separately from raw status counts.
- Table-level status uses authored SHA precedence and MD5-only matching without
  an N+1 preferred-chart lookup or table placeholders.
- Windows-style stored folders and path-only fallback records both match the
  parent query. Its query-plan test requires the folder index, not a full scan.

## Verification

- Desktop `cmake --build cmake-build-debug --target main -j 6`: passed.
- Focused select/repository/draw-command checks: 24 passed.
- Final `ctest --test-dir cmake-build-debug --output-on-failure -j 6`:
  319/319 passed (36.85 seconds).
- Independent review: all three findings resolved, no further correctness
  changes requested. `git diff --check` passed.
- An initially missing movie-catalog test binary was built. The unchanged
  built-in renderer timing golden check failed on intermediate runs and passed
  on the final full run; no golden fixture was updated.
- Verification is automated; no manual skin UI acceptance or mobile build was
  performed, and nothing was deployed.
