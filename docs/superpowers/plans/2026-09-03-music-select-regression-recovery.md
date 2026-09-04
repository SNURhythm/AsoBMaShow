# Music-select regression recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore skin search touch, Beatoraja selector sorting and
difficulty-table ordering, and action-capable retained Records modal behavior.

**Architecture:** `TextInputBox` owns the invariant that its native editor is
started from its current Yoga frame. The selector bar manager copies
Beatoraja's root sortable default and its stable `SongBar`/`FolderBar`
comparator participation. A shared retained Records modal replaces the
display-only selector overlay and supplies the existing Main Menu action set to
both scenes through owner-provided scene-transition callbacks.

**Tech Stack:** C++23, SDL2, Yoga, bgfx, existing replay consumers/exporter,
CTest.

**Spec:** `docs/superpowers/specs/2026-09-03-music-select-regression-recovery.md`

## Global Constraints

- `/Users/xf/workspace/SNURhythm/beatoraja` commit
  `c2ed5db1a46145ed10790c3872f717e95b59db9d` is the compatibility authority.
- Do not add skin budgets, parser validation, fallbacks, or unsupported
  compatibility behavior.
- Preserve retained Music Select state when an action leaves gameplay and
  returns.
- Keep commits focused and independently testable.

---

### Task 1: Frame the native skin text editor before beginning input

**Files:**
- Modify: `src/view/TextInputBox.cpp:142-150`
- Test: `tests/text_input_box_tests.cpp`

**Interfaces:**
- Produces: `TextInputBox::beginEditing()` begins from the latest declared
  Yoga frame.

- [x] **Step 1: Write the failing test**

```cpp
input.setSize(240, 52);
input.setPositionNoLayout(450, 735, YGPositionTypeAbsolute);
input.beginEditing();
SDL_Rect nativeRect{};
SDL_GetTextInputRect(&nativeRect);
expect(nativeRect.x == 450 && nativeRect.y == 735 &&
       nativeRect.w == 240 && nativeRect.h == 52,
       "begin editing uses the latest declared input frame");
```

- [x] **Step 2: Run the test to verify it fails**

Run: `ctest --test-dir cmake-build-debug -R '^text_input_box_tests$' --output-on-failure`

Expected: the native rectangle is stale before an explicit layout pass.

- [x] **Step 3: Write the minimal implementation**

```cpp
void TextInputBox::beginEditing() {
  if (isSelected) return;
  applyYogaLayout();
  onSelected();
  // existing platform text-input startup
}
```

- [x] **Step 4: Run the focused test to verify it passes**

Run: `ctest --test-dir cmake-build-debug -R '^text_input_box_tests$' --output-on-failure`

- [x] **Step 5: Commit**

Commit message: `Fix skinned search touch frame`

### Task 2: Match Beatoraja sort participation and table ordering

**Files:**
- Modify: `src/music_select/MusicSelectBarManager.cpp:92-180,270-322`
- Modify: `src/music_select/MusicSelectRepositoryProjection.cpp:535-570`
- Test: `tests/music_select_bar_manager_tests.cpp`
- Test: `tests/music_select_repository_projection_tests.cpp`

**Interfaces:**
- Produces: root rows participate in sorting; only Song and Folder bars
  participate in source comparisons; table and hash folders stay stably in
  metadata order while HashBar songs follow `skinSortId`.

- [x] **Step 1: Write failing tests**

```cpp
require(ids(manager.snapshot()) ==
            std::vector<std::string>{"folder:alpha", "folder:zulu",
                                     "table:first", "table:second"},
        "root sorting preserves authored TableBar order");
require(ids(levelManager.snapshot()) ==
            std::vector<std::string>{"hash:12", "hash:1"},
        "table levels preserve difficulty metadata order");
require(ids(hashManager.snapshot()) ==
            std::vector<std::string>{"song:alpha", "song:zulu"},
        "HashBar song children follow the selected skin sort");
```

- [x] **Step 2: Run tests to verify failure**

Run: `ctest --test-dir cmake-build-debug -R '^(music_select_bar_manager_tests|music_select_repository_projection_tests)$' --output-on-failure`

Expected: root sorting is skipped, table/hash bars are title-compared, and
HashBar children are marked non-sortable.

- [x] **Step 3: Implement the source behavior**

```cpp
bool sourceSorterParticipates(const MusicSelectBar &bar) {
  return bar.kind == MusicSelectBarKind::Song ||
         bar.kind == MusicSelectBarKind::Folder;
}
// compare non-participating pairs as equal; place them after participating
// bars; use the selected source sort only for participating SongBars.
```

Initialize root sorting as enabled, replace it with the selected directory's
`sortable` value, and mark projected Table and Hash directories sortable.

- [x] **Step 4: Run focused tests to verify pass**

Run: `ctest --test-dir cmake-build-debug -R '^(music_select_bar_manager_tests|music_select_repository_projection_tests)$' --output-on-failure`

- [x] **Step 5: Commit**

Commit message: `Match Beatoraja selector sorting`

### Task 3: Share the action-capable Records modal

**Files:**
- Create: `src/scene/ReplayRecordsModal.h`
- Create: `src/scene/ReplayRecordsModal.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/MusicSelectScene.h`
- Modify: `src/scene/MusicSelectScene.cpp`
- Test: `tests/replay_records_modal_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ReplayRecordsModal` retained in a parent view, with
  `showChart(ChartMetaRecord)`, `hide()`, `update()`, `handleEvents()`, and
  owner callbacks for replay watch, replay export, result recall, IR upload,
  file share, and delete.

- [x] **Step 1: Write failing modal action tests**

```cpp
modal.showChart(record);
modal.selectRecord(modernRecord);
modal.activate(ReplayRecordsModalAction::Watch);
expect(watched == modernRecord.result.attemptId,
       "selected modern record requests replay watch through its owner");
modal.activate(ReplayRecordsModalAction::VideoExport);
expect(exportRequested == modernRecord.result.attemptId,
       "selected modern record requests video export through its owner");
```

- [x] **Step 2: Run the test to verify it fails**

Run: `cmake --build cmake-build-debug --target replay_records_modal_tests -j 6 && ctest --test-dir cmake-build-debug -R '^replay_records_modal_tests$' --output-on-failure`

Expected: the shared modal target and action boundary do not exist.

- [x] **Step 3: Extract the existing Main Menu Records UI and action state**

Move the retained replay list, selection, filter/sort, watch options, export
options/progress, file share/delete, and action visibility logic into
`ReplayRecordsModal`. Preserve existing record merging and replay-action
predicates. Have Main Menu construct it with its existing launch/export/recall
handlers rather than reimplementing them.

- [x] **Step 4: Attach the same modal to Music Select**

Replace the display-only `chartRecordsModal_` with `ReplayRecordsModal`.
Supply retained-selector return-target launch handlers so Watch and Export
leave and return without losing selector state. Route Escape and modal events
to the shared owner before skin input.

- [x] **Step 5: Run focused tests to verify pass**

Run: `ctest --test-dir cmake-build-debug -R '^(replay_records_modal_tests|replay_summary_list_view_tests|music_select_toolbar_view_tests)$' --output-on-failure`

- [x] **Step 6: Commit**

Commit message: `Share action-capable records modal`

### Task 4: Verify and hand off

**Files:**
- Verify only.

- [ ] **Step 1: Check whitespace and review changed boundaries**

Run: `git diff --check develop...HEAD`

- [ ] **Step 2: Build the desktop target**

Run: `cmake --build cmake-build-debug --target main -j 6`

- [ ] **Step 3: Run the complete suite**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -j 6`

- [ ] **Step 4: Push and request review**

Push the feature branch, request `@codex review` on PR #103, and resolve any
review findings before completion.
