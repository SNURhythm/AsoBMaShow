# Gameplay Rendering Follow-ups

> Required workflow: subagent-driven development. The user authorized all findings and previously requested parallel implementation. Work in the existing checkout with disjoint ownership; parent owns builds, integration review, commits and pushes.

**Goal:** Implement all three measured gameplay rendering findings from [the audit](../../reviews/2026-10-28-gameplay-rendering-audit.md), preserving rendered pixels, presentation lifetime and gameplay semantics.

**Baseline:** `501a6d60` contains the separately verified broader performance follow-ups.

## Global constraints

- No new worktree, deployment, parser edit or whole-file formatting. Keep unrelated files untouched.
- Parent alone runs CMake/Ninja/Xcode, stages, commits and pushes. No concurrent builds in one directory. Workers do not delegate.
- Preserve source compatibility/behavior where practical; update all affected callers and tests when storage contracts change.
- Prefer bounded fixtures, deterministic work/allocation counts and existing behavioral tests. Use real Metal readback for BGA pixel equivalence and native/frame-lifetime checks where affected.
- Cache only verified complete GPU output, never a failed or discarded submission. Respect pending iOS frame/lifecycle/export ownership.
- Do not claim input latency or phone FPS improvement from component work measurements.

### Task 1: Reuse unchanged fullscreen BGA composition

**Ownership:** `src/main.cpp`, `src/audio/Jukebox{.h,.cpp}`, `src/audio/GameplayBgaFrame.h`, BGA/video backend interfaces/implementations needed for truthful content identity, `src/rendering/BlurPass{.h,.cpp}` and a narrowly scoped rendering cache helper if useful; existing BGA/video/Metal tests. Avoid PlayfieldProjection/Pacemaker/state-store files. Coordinate central build registration through parent.

- [x] Add safe content identity for prepared base/layer/miss surfaces. Static images and repeated decoded software/native-video frames must be reusable. Actual new decoded frames, resource replacement, seeks/reset/reload and lifetime transitions must invalidate. An authored resource ID, frame serial, texture handle or reusable address alone is insufficient.
- [x] Reuse fullscreen composition/blur output while content and relevant settings/targets are unchanged; keep final draw and brightness live. Handle layered/miss/blank transitions and ordinary fallback correctly. Legacy unprepared paths may conservatively recompute.
- [x] Invalidate on viewport/display-mode/blur/downsample/target or renderer recreation and export/pending-frame cancellation boundaries. Preserve explicit prepared-frame lease consumption. Do not mark output cached when geometry allocation, upload, composition or blur submission fails.
- [x] Cover unchanged/replaced images, repeated/new video frames, seeks, role transitions, settings/resize/target recreation, transient failure recovery and lease release. Compare real Metal pixels and submission counts. Parent repeats bounded production probe and relevant renderer/video tests, desktop build, and Apple compile checks if backend interfaces require them.

### Task 2: Bound PMS late-POOR cursor traversal

**Ownership:** `src/scene/play/PlayfieldProjection{.h,.cpp}`, `tests/playfield_projection_tests.cpp` and narrowly related existing projection tests. No Jukebox, Pacemaker or main edits.

- [x] Stop cursor traversal at the first future timeline or retained barrier without changing the previous-row cursor convention.
- [x] Preserve strict time boundaries, long-head/tail retention, show-past notes, equal-time ordering, descent behavior, rewind/reset and jump/catch-up behavior.
- [x] Extend focused behavioral and work-count coverage for fixed visible work with large future/retained suffixes. Parent runs projection tests and existing production projection probe; require no future-chart-count scaling.

### Task 3: Share immutable pacemaker progression storage

**Ownership:** `src/scene/play/Pacemaker.h`, `src/scene/play/PlayfieldVisualState{.h,.cpp}`, pacemaker target consumers and existing state/skin/replay tests that need adaptation. `GamePlayScene{.h,.cpp}` and `PlaySkinStateBridge{.h,.cpp}` are available if needed. No projection/Jukebox/BlurPass/main edits.

- [x] Replace repeated owning progression vector copies with immutable shared storage, preserving target scalar fields and indexing/fallback behavior. Prefer a small explicit representation over complex mutable copy-on-write machinery.
- [x] Preserve captured-frame immutability when targets are replaced or reset; retain correct BEST/rank/off/replay/course behavior and meaningful value equality.
- [x] Update all production/test consumers and add bounded sharing/lifetime/allocation coverage through actual authority store/capture and skin bridge where appropriate. Parent runs affected tests and repeats the production capture probe; chart-sized allocation/copy work must disappear.

### Task 4: Integration and completion

- [x] Independently review each task and combined changes; resolve actionable findings.
- [x] Run affected combined checks and required app/platform builds, record measured before/after component results, update audits and PR 161, commit and push all completed work.
