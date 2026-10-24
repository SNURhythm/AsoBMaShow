# Beatoraja 0.8.9 Compatibility Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Apply the native-relevant changes introduced between beatoraja `ad42f56c` and `83574048`, preserving existing application features and resource ownership.

**Architecture:** Extend the existing C++ skin decoder, selector, renderer and Lua host. Keep network jobs off the Lua/render thread, publish input through immutable snapshots, and use native filesystem/platform services. Historical oracle snapshots remain pinned; new behavioral tests identify the new upstream contract.

**Tech Stack:** C++23, Lua, libcurl/Foundation, SDL, existing CMake/CTest targets.

**Spec:** [Filtered upstream audit and adoption recommendations](../../audits/2026-10-24-beatoraja-upstream-update.md). The user authorized applying applicable changes after reviewing that report.

## Global constraints

- Work in the current `feature/beatoraja-0-8-9-compat` checkout; no new worktrees.
- Use bounded memory/queues, metadata-only archive operations, existing private skin storage, and native platform boundaries.
- Preserve preview/export restrictions, historic provenance and oracle pins.
- No unrelated Java-specific rewrites, deployments, or parser edits.
- Extend existing tests and build one target set at a time with `-j 6`.

## Review focus

- Missing charts with retained scores and mixed folder rows must sort consistently.
- Practice range/cursor changes must apply only in the active practice menu.
- New input edges must expire across frame publications; retained held input must not launch a browser repeatedly.
- HTTP bounds count raw bytes including newlines/UTF-8, and cancellation/disposal must release resources without touching destroyed Lua/runtime objects.
- File facade paths must be revalidated after creation; deletion must not follow a link or remove a directory.

### Task 1: Decoder, sorting, practice graphs

**Files:** `src/skin/beatoraja/LuaSkinTableDecoder.cpp`, `src/music_select/MusicSelectBarManager.cpp`, `src/skin/beatoraja/Skin2DRenderer.cpp`; corresponding existing decoder, selector and draw-command tests.

**Interfaces:** Preserve string fallback paths in `decodeRawFontFallback`; apply a missing-song comparison before `sourceCompare`'s individual sorter rules; pass `ISkinFrameState::practiceState()` range and frequency to authored note-distribution preparation.

- [x] Extend existing fixtures/assertions for string font paths, every sorter with missing songs, and practice range/cursor at 150%.
- [x] Build/run the affected test targets and observe the pre-fix failures.
- [x] Implement the three changes; check paged/SQL selection paths and scalable fallback consumption.
- [x] Run affected targets and review the diff; include in the final integration commit.

### Task 2: File compatibility and direct helper containment

**Files:** `LuaSkinFileSystem.{h,cpp}`, `LuaSkinHostModules.cpp` and existing filesystem/host tests under `src/skin/beatoraja` and `tests`.

**Interfaces:** Add regular-file deletion and file/directory classification through the filesystem; expose `main_state.file_delete`, File `newInstance`, name/path/type/tostring methods and File-valued `listFiles`. Match the updated direct helper no-follow contract without silently changing unrelated legacy `io`/resource policies.

- [x] Add failing host tests for new methods and File table results; add filesystem cases for links, directories, missing files and preview denial.
- [x] Implement typed filesystem operations and minimal Lua facades using existing error/capability conventions.
- [x] Run filesystem/host tests and check old expectations against the changed upstream contract; include in the final integration commit.

### Task 3: Input snapshots and browser action

**Files:** `src/input/InputTypes.h` and its snapshot producer, `LuaSkinLegacyInputHost.{h,cpp}`, `LuaSkinHostModules.{h,cpp}`, runtime/document-load application wiring and existing input/host tests.

**Interfaces:** Publish just-pressed keys, mouse buttons, pointer position/delta and touch edges alongside existing input state. Expose upstream Gdx methods/Buttons and Java constant-name resolution. Route `main_state.open_url` through an injected native browser action with HTTP(S)-only validation, current input-edge requirement and one-second per-skin limit; disable in preview/export.

- [x] Test new facade values, edge expiry, key naming, URL validation/rate limit and denied contexts before implementing.
- [x] Extend the producer/consumer snapshots and host actions without live SDL access from Lua.
- [x] Run affected input/host/runtime tests and verify production scene wiring; include in the final integration commit.

### Task 4: HTTP byte limits and bounded asynchronous requests

**Files:** `LuaSkinHttpClient.{h,cpp}`, `LuaSkinCurlHttpTransport.cpp`, `LuaSkinFoundationHttpTransport.mm`, new focused async helper if needed, `LuaSkinHostModules.cpp`, build registration and existing HTTP tests.

**Interfaces:** Replace line/character limits with 1 MiB default and 8 MiB maximum raw bytes; support optional synchronous capacity and legacy `setResponseLimitBytes`/reader close. Expose `http_get_async`, `http_poll`, `http_cancel` with upstream result shape and consume-once semantics. Two shared workers, 32 queued jobs, 16 retained jobs/results per skin, 16 MiB reservations/results budget, 1–5000 ms total async deadline, at most three validated redirects, public destinations only and no HTTPS downgrade. Workers own connections/value state and never invoke Lua.

- [x] Change/add tests for UTF-8/newline byte boundaries, unbounded line count within bytes, malformed capacity, legacy close and both transport implementations.
- [x] Implement synchronous contract, then observe failing async lifecycle/queue/budget tests before implementing the job owner.
- [x] Wire cancellation/deadlines and restricted async destination policy into transports; cancel all jobs on skin disposal and deny preview/export submissions.
- [x] Run host, HTTP transport and lifecycle tests and review ownership; include in the final integration commit.

### Task 5: Assess follow-ups and finish

**Files:** Upstream audit plus any native files where the optional report follow-ups establish a concrete compatible change.

**Interfaces:** Preserve existing numeric sort IDs and score provenance if extending configurable cycling or measured duration. Reuse existing native diagnostic counters instead of adding Java-style instrumentation.

- [x] Record a disposition for sorter configuration, measured duration, native lifecycle diagnostics, malformed-data cases and ASIO bounds; implement demonstrated applicable gaps with focused behavioral coverage.
- [x] Run combined affected CTest groups and desktop `main` build; broaden verification only if impact warrants it.
- [x] Obtain one independent whole-branch review, resolve substantive findings, and update the audit with implementation/verification status.
- [x] Commit and push all task-related changes to the current upstream.
