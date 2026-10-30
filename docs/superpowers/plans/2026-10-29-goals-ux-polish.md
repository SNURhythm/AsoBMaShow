# Goal UX Polish Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development for this user-authorized parallel implementation. Parent owns integration, commits, pushes, and the shared build directory.

**Goal:** Make goal navigation predictable, chart/folder behavior consistent, and compact layouts usable.
**Architecture:** Keep the existing scenes and repositories. Share small card/layout helpers; each scene owns its navigation state and source picker. Stable goal identities and achievement handling remain intact.
**Tech Stack:** C++20, SDL/bgfx native UI, Yoga, existing Python-extracted scene tests and CTest.
**Spec:** docs/superpowers/specs/2026-10-29-goals-ux-polish-design.md

## Global Constraints

- No new worktrees, dependencies, deployment, or whole-file formatting.
- Only parent runs CMake/Ninja in cmake-build-debug and commits/pushes.
- Agents own disjoint files; request cross-scope changes from parent.
- Existing folder goal identity is fixed when editing; Add never overwrites an occupied identity.
- Native screenshot capture is unavailable; do not claim rendered visual verification.

## Shared Interfaces

Shared agent owns src/goals/GoalNavigation.h and new src/view/GoalListRow.h.
- `goal_navigation::stackedNavigation(float availableWidth, bool compact)` selects top navigation for compact screens or width below 900 UI units.
- `goal_navigation::columns(float width, bool navigation, bool stacked = false)` preserves existing desktop behavior; stacked content has zero side navigation width and centered body capped at 960.
- State adds `chartOffset`, `chartAchievedOffset`, `folderOffset`, `folderAchievedOffset` ints; `chartScrollOffsets`, `folderScrollOffsets` maps; `chartNavigationOffset`, `folderNavigationOffset` floats. Existing fields retained.
- `goal_ui::clickableRow(View *parent, std::function<void()> onOpen) -> View *` appends a full-width auto-height clickable card and returns the inner vertical content container. Consumer adds passive content only; row owns the opening action.

## Task 1 — Chart scene

Own src/scene/ChartGoalsScene.{h,cpp}, tests/chart_goals_scene_{fixture.cpp,tests.py} and chart_goals_navigation_fixture.cpp.
- [x] Extend behavioral tests for actual-origin Back, editor cancel, picker sorting, and empty group actions.
- [x] Implement routing, list/detail hierarchy, compact top navigation and shared state restoration.
- [x] Use shared clickable-row helper; request localization keys from parent.
- [x] Run focused Python scene tests only; report result and review risks.

## Task 2 — Folder scene

Own src/scene/FolderGoalsScene.{h,cpp}, tests/folder_goals_scene_tests.py and any folder-only fixture.
- [x] Extend behavioral tests for Average EX single-goal entry, multiple-goal picker, Back, cancelled drafts, preserved scope changes, occupied identities and scroll state.
- [x] Implement explicit source picker/new/edit navigation, draft preservation, compact navigation, clearer row/detail/empty states and destructive removal.
- [x] Use shared clickable-row helper; request localization keys from parent.
- [x] Run focused Python scene tests only; report result and review risks.

## Task 3 — Shared layout and cards

Own src/goals/GoalNavigation.h, src/view/GoalListRow.h, src/view/GoalKindCycleButton.h; add tests in tests/goal_navigation_layout_tests.py with registration handled by parent.
- [x] Implement shared interfaces and concise scope cue/accessibility naming while preserving the cycle design.
- [x] Test desktop centering, compact widths, and row layout/click behavior where meaningful.
- [x] Provide integration notes to scene agents; do not edit their files.

## Task 4 — Integration

Parent owns localization, docs, registration, necessary selector Browse callbacks, final review and verification.
- [x] Integrate browse-return actions with the originating selector without accidental scene destruction.
- [x] Add localized strings in sorted catalog order; update feature documentation.
- [x] Review all changes, run relevant scene/layout/localization tests, and build desktop main at -j 6.
- [x] Obtain a fresh whole-change review and fix findings; prepare the verified changes for PR #164.

## Review Focus

Profile changes during deferred actions; a source with only Average EX goals; an occupied folder goal slot; empty/achieved-only lists; phone portrait/landscape and long localized labels. Tests belong to the task owning each path. No duplication of large synthetic fixtures.

## Verification outcome

- Desktop `main` build passed at `-j 6`.
- Chart scene, folder scene, shared Yoga layout, localization catalog, and native localization CTests passed (5/5).
- Fresh review findings fixed: horizontal action sizing and cancellation after deleting the sole chart goal. The latter reproduced with an added behavioral regression before its fix.
- Safe-area width now drives chart navigation breakpoints consistently.
- Native screenshot inspection remains unavailable; no deployment performed.
