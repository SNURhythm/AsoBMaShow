# Settings Clarity Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Follow the user's existing approval to implement all six improvements.

**Goal:** Make settings scope, location, dependencies and reversible appearance editing understandable.
**Architecture:** Retain the native view builders and profile storage. Add small pure helpers for section mutation and search routing; reuse shared editors between settings and preview, and keep custom-skin writes within the existing controller.
**Tech Stack:** C++/SDL/bgfx native views, CMake/CTest, Messages.inc localization.
**Spec:** docs/superpowers/specs/2026-11-14-settings-clarity-design.md

## Global Constraints

- Do not create a worktree or deploy. Leave the unrelated Xcode project change untouched.
- Preserve saved meaning, all supported locales, feature-disabled builds and existing theme/control patterns.
- Run one build per build directory. Use focused tests and main at -j 6.

## Review Focus

- Shared controls must not imply per-mode or per-skin ownership.
- Copy/reset must preserve unrelated settings and source orientation, and honor busy/profile/target changes.
- Search must keep focus while typing and must not change saved skin selection.
- Disabled custom-skin builds still expose built-in appearance and shared overlays.
- Failed imports, active operations and actionable diagnostics must remain accessible.

### Task 1: Scope-safe editing and navigation model

Files: new focused helpers under src/settings or src/scene; tests and cmake/TestGroupCatalog.cmake; GameplaySkinSettingsController.*.

- [x] Add behavioral coverage for appearance-section copying/resetting, unrelated-field preservation, orientation defaults, and custom-skin controller writes.
- [x] Add localized search descriptors with destinations and test alias/localized matching and routing.
- [x] Run failing tests, implement pure helpers and controller operations, then run focused passing checks.

### Task 2: Native settings organization

Files: SettingsScene.h, SettingsSceneLayout.cpp, SettingsSceneControls.cpp, SettingsSceneSkins.cpp, SettingsSceneSkinsUnavailable.cpp, new focused SettingsScene organization implementation if needed, Messages.inc.

- [x] Rename navigation and add active-profile/group scope labels.
- [x] Separate built-in HUD and shared overlay editing; expose common builders in settings and preview.
- [x] Put skin editing first and move maintenance/detail UI to clearly named sections while retaining error/operation visibility.
- [x] Add timing dependency hints based on available metadata.
- [x] Wire scoped reset/copy confirmations and searchable destinations without broad profile mutations.
- [x] Update existing UI contracts/behavioral tests proportionally, including fallback builds and focus/lifetime concerns.

### Task 3: Verification and handoff

- [x] Build main and affected settings/skin tests in one invocation; run focused CTest groups and localization checks.
- [x] Review the final diff and perform a fresh independent code review, fixing findings and verifying changed paths.
- [x] Update settings documentation, commit only related files, push the branch, link PR #188 immediately after push, and update its description.

## Verification

Desktop `main` build passed. All 13 `area_settings` tests and five focused
profile/skin/localization tests passed after review fixes. Custom-skin-disabled
syntax checks passed for the shared editors and fallback screen. Independent
review findings (preview page routing, persistent errors, exact timing search
aliases) were fixed with failing-then-passing behavioral coverage. Native GUI
inspection was unavailable because the local `orca` command was not installed.
