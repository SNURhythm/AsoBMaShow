# Performance Follow-ups Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Tasks run in parallel when their file ownership is disjoint, as explicitly requested by the user. Parent owns builds, integration, reviews and commits.

**Goal:** Remove confirmed avoidable library, settings, gameplay and retained UI work while separately investigating the gameplay rendering pipeline.

**Architecture:** Preserve existing visible behavior and timing rules. Narrow library queries and avoid unnecessary category sampling; coalesce slider persistence; make HCN integration depend on eligible heads; avoid redundant retained-row and warm-atlas preparation with correct invalidation.

**Tech Stack:** C++23, SQLite, SDL3, Yoga, bgfx, native CMake tests.

**Spec:** `docs/reviews/2026-10-28-performance-audit.md` and the user's request to fix these findings in parallel while investigating gameplay rendering.

## Global Constraints

- Work in the existing checkout and branch. No new worktrees or deployments.
- Preserve gameplay outcomes, uniform random selection, query membership/order, profile ownership/durability, UI geometry and authored skin behavior.
- Parent alone runs CMake/Ninja builds, edits central CMake/test registration, stages, commits and pushes. Workers must not revert other workers' edits.
- Prefer extensions to existing behavioral tests and bounded fixtures. No whole-file formatting, parser changes, or private-library scanning.
- Existing renderer allocation/BGA/upload fixes and presentation experiments are not new findings. GPU-work improvements do not establish input-to-display latency improvements.

## Review Focus

- Query identity/order under tied sort keys, NULLs, missing difficulty entries and score-dependent sorts.
- Category invalidation after score/filter/library changes, including a category that becomes empty.
- Slider interruption, scene/profile transitions and failed saves must not lose the final value or persist into a different profile.
- HCN catch-up/early tails/overlap must preserve strict thresholds and time/note-ID ordering.
- Retained UI dimensions, hit testing, dirty-layout publication, Lua side effects and atlas replacement must remain correct.

### Task 1: Library query work

**Files:** `src/repositories/ChartRepository{.h,Queries.cpp}`, `src/scene/MainMenu{Scene.h,Scene.cpp,Library.h,Library.cpp}`, existing repository/menu tests.

**Interfaces:** Keep `FindChartMetaIndex` and true random selection semantics. Add a separate category-availability API if needed; only this task owns its repository and menu callers.

- [x] Extend existing query tests to compare non-default lookup with actual rows and assert bounded rich hydration, including ties/missing targets and difficulty/score variants.
- [x] Replace full rich-row index lookup with a narrow query sharing the existing filter/order rules.
- [x] Separate category existence from reservoir sampling. Remove unnecessary ordering; stop when availability thresholds are met. Reuse availability across sort-only reloads and invalidate it for membership/score changes.
- [x] Extend existing random/menu tests for availability and invalidation. Parent builds/runs relevant repository/menu targets and repeats the bounded actual-Session probe.

### Task 2: Rate-slider persistence

**Files:** `src/scene/MusicPlayerScene{.h,.cpp}`, `src/view/SnappedSlider{.h,.cpp}`, existing slider/music-player tests; a small scene-local helper only if useful.

**Interfaces:** Preserve live rate feedback and the slider's existing value-change callback. Add an interaction-completion callback or equivalent scoped persistence mechanism without changing unrelated settings persistence.

- [x] Add behavioral coverage for multiple drag changes with one final persistence notification, interruption/cancel/focus loss, and single-click/touch changes.
- [x] Coalesce only persistence while retaining live playback-rate changes. Save final values on relevant exits and preserve profile identity and error reporting.
- [x] Parent builds/runs focused slider/music-player tests and verifies app compilation.

### Task 3: HCN integration eligibility

**Files:** `src/scene/play/GameplaySimulation{.h,.cpp}`, related gameplay definition files only if necessary, `tests/gameplay_automatic_authority_tests.cpp` and existing worker tests.

**Interfaces:** Keep simulation public results and LR2 behavior unchanged; maintain private admission/expiration/active bookkeeping for non-LR2 HCN integration.

- [x] Extend behavior coverage for future/expired heads with fixed active count, overlaps, strict thresholds, early successful release and one-shot versus chunked advancement.
- [x] Replace repeated all-head scans with an eligible-head path; preserve exposed balances and chronological/note-ID ordering.
- [x] Parent runs focused gameplay tests and the actual production `advanceTo` probe; require scaling improvement without outcome changes.

### Task 4: Retained recycler motion

**Files:** `src/view/RecyclerView.h`, `src/view/View{.h,.cpp}` only if necessary, existing layout/recycler tests.

**Interfaces:** Preserve current sizing-before-bind and event/layout contracts.

- [x] Count layouts, binds and item copies across same-range motion, boundary crossings and resize using existing fixtures.
- [x] Add a same-range/same-size/no-rebind motion path; avoid record/container rebuilds and unnecessary layout publication while preserving dirty state and hit testing.
- [x] Parent runs affected UI tests and a bounded work-count/performance probe with representative row trees.

### Task 5: Music-select atlas preparation

**Files:** `src/skin/beatoraja/MusicSelectSkinSession{.h,.cpp}`, a small text/residency helper if needed, existing skin/renderer tests. Avoid overlapping shared View code.

**Interfaces:** Preserve Lua evaluation phases and animated destinations. Prefer bounded successful exact-string/atlas-generation residency caching and title-only gathering to unsafe cross-phase plan sharing.

- [x] Verify warm repeated-frame work with an existing/synthetic session fixture.
- [x] Remove duplicate preparation that can be eliminated without changing callback ordering; invalidate on string/atlas changes and keep memory bounded.
- [x] Parent runs focused skin tests/benchmark and checks cold/missing-glyph behavior.

### Task 6: Gameplay rendering investigation and integration

**Files:** Parent read-only investigation of `GamePlayScene`, gameplay skins, `RenderContext`, shader/texture submission, BGA and bgfx integration; findings recorded in a review document.

- [x] Trace actual built-in and custom-skin gameplay render callers, cadence, culling, uploads, draw/state changes and retained data. Check current caches and prior fixes.
- [x] Validate only strong candidates with bounded production measurements; distinguish CPU/GPU work from presentation latency. Do not change rendering merely on suspicion.
- [x] Independently review each implementation task and then the combined changes; resolve actionable findings.
- [x] Run the appropriate combined subsystem checks and desktop app build, record measured before/after results, commit/push task-related changes and update linked PR 161.

Validation detail: broad UI checks and the full skin-session suite passed after correcting extracted-fixture integration. Recycler verification uses a bounded structural row fixture and exact work counts; no native chart/music-row wall-clock or device FPS claim is made.

Final integration review found and resolved a post-callback clean-Yoga publication case. The scoped re-review passes. The final desktop build and both affected layout/recycler tests pass, including the four root/descendant callback variants and zero-layout ordinary movement. The additional rendering findings are authorized for a separate implementation follow-up.
