# Independent Folder Goals Implementation Plan

> Execute natively in the current checkout; no worktree or deployment.

**Goal:** Support independent folder goals in identical source/type/LN scopes without repeating the chart migration failure.
**Architecture:** Preserve the existing text-key table; add stable IDs to the model and versioned payloads. Route scene actions by ID and preserve legacy achievement keys.
**Tech Stack:** C++23, SQLite, native Yoga UI, existing CTest suites.
**Spec:** docs/superpowers/specs/2026-10-29-independent-folder-goals-design.md

## Constraints and review focus

- No table rename/rebuild; existing IDs/history survive.
- New IDs become visible to callers only after successful commit.
- Missing or mismatched IDs cannot overwrite another goal or resurrect one.
- Same-scope simultaneous achievements receive independent credit once.
- Private profile checks use temporary copies only; keep CI synthetic.

## Tasks

- [x] Extend `tests/folder_goal_tests.cpp` with same-scope storage isolation and legacy add/edit tests containing the old invalid score trigger; observe the current overwrite failure.
- [x] Update `FolderGoal.{h,cpp}` and `FolderGoalStore.{h,cpp}`: `Goal::id`, legacy `scopeKey`, persistent `key`, version-1 loading, version-2 writes, random new IDs and update-only existing saves.
- [x] Update `ScoreRepository::SaveFolderGoal(Goal &, error)` to match by ID, enforce source/mode/type identity, preserve dates and return IDs after commit; extend repository/Kombo integration coverage.
- [x] Change `FolderGoalsScene::choose(Goal)` and its extracted tests to select/cancel by ID; remove occupied-scope blocking and obsolete translation.
- [x] Update status/bubble summaries and feature docs for multiple same-type goals; keep all-achieved aggregation.
- [x] Run folder/scene/localization/repository tests and desktop `main` build; verify a private profile copy and iOS SQLite upgrade path.
- [x] Obtain fresh focused review and prepare verified changes for PR #164.

## Verification

- Existing overwrite reproduced: the second same-scope save left one row instead of two.
- Eight focused CTest suites passed: folder storage, folder scenes, chart storage, chart scenes, repository/achievement integration, score-cache queries, localization catalog, and native localization.
- Desktop `main` build passed at `-j 6`.
- Production folder storage tests passed on iOS Simulator with bundled SQLite, including both add-first/edit-first version-1 upgrades containing the invalid attached-score trigger. The fixture confirms table renaming would fail, while independent saves leave the schema version unchanged.
- A temporary copy of the supplied profile passed add/edit/reopen/remove in iOS Simulator. Existing folder metadata/history, chart-goal payloads, score count, and schema version were preserved. Baseline DB/WAL/SHM checksums remained unchanged; no private data included.
- Fresh independent review reported no actionable defects. No deployment performed.
