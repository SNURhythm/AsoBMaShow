# iOS Startup Default Library Folder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Start iOS with AsoBMaShow's persisted default BMS folder instead of presenting an automatic folder picker when the chart library has no entries.

**Architecture:** Add a pure platform-to-bootstrap policy beside the existing main-menu library helpers, cover it with the focused unit target, and make the empty-library refresh branch follow that policy. Mobile targets bootstrap `Documents/BMS`; desktop targets retain the existing picker/console flow. The separate iOS Add Folder action remains the only caller that presents the native folder picker.

**Tech Stack:** C++23, CMake/CTest, Objective-C++ iOS build verification, SQLite-backed `ChartRepository`

## Global Constraints

- iOS startup must not call `PickIOSFolder()` automatically.
- iOS must create and persist `ChartRepository::DefaultBmsFolderPath()` when no effective entries exist.
- Existing saved paths and iOS security-scoped bookmarks must remain unchanged.
- `MainMenuScene::addIOSFolderEntryFromFiles()` and the Add Folder button must remain unchanged.
- Android keeps default-folder startup behavior; desktop keeps its folder-picker and console fallback.
- Folder creation, entry insertion, or post-insertion reload failure must fail the library task without opening a picker.
- Do not edit `src/iOSNatives.mm`, repository schemas, Android permissions, or Settings UI.
- Use `[skip ci]` in implementation commit messages.
- Do not run an upload deployment; iOS verification must use `scripts/ios_firebase_deploy.sh --build-only`.

---

### Task 1: Make empty-library startup platform policy explicit and use it

**Files:**
- Modify: `src/scene/MainMenuLibrary.h`
- Modify: `tests/main_menu_library_tests.cpp`
- Modify: `src/scene/MainMenuScene.cpp:1945-2015`

**Interfaces:**
- Consumes: `TargetPlatform` and `TARGET_PLATFORM` from `src/targets.h`; `ChartRepository::DefaultBmsFolderPath()`; `ChartRepository::Session::InsertEntry()` and `SelectEffectiveEntries()`
- Produces: `main_menu_library::EmptyLibraryBootstrapMode` and `main_menu_library::emptyLibraryBootstrapMode(TargetPlatform) noexcept`

- [ ] **Step 1: Add failing platform-policy assertions**

Add this include to `src/scene/MainMenuLibrary.h` immediately after the existing `LibraryFolderClearData.h` include:

```cpp
#include "../targets.h"
```

Do not add the production enum or function yet. Add these assertions near the start of `main()` in `tests/main_menu_library_tests.cpp`, before opening the SQLite database:

```cpp
  ASSERT_EQ(
      static_cast<int>(
          main_menu_library::EmptyLibraryBootstrapMode::DefaultFolder),
      static_cast<int>(main_menu_library::emptyLibraryBootstrapMode(
          TargetPlatform::iOS)),
      "iOS empty library uses the default folder");
  ASSERT_EQ(
      static_cast<int>(
          main_menu_library::EmptyLibraryBootstrapMode::DefaultFolder),
      static_cast<int>(main_menu_library::emptyLibraryBootstrapMode(
          TargetPlatform::Android)),
      "Android empty library uses the default folder");
  ASSERT_EQ(
      static_cast<int>(
          main_menu_library::EmptyLibraryBootstrapMode::FolderPicker),
      static_cast<int>(main_menu_library::emptyLibraryBootstrapMode(
          TargetPlatform::MacOS)),
      "desktop empty library uses the folder picker");
```

- [ ] **Step 2: Build the focused test to verify it fails**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests -j 6
```

Expected: compilation fails because `EmptyLibraryBootstrapMode` and `emptyLibraryBootstrapMode` are not members of `main_menu_library`.

- [ ] **Step 3: Implement the minimal pure startup policy**

Add this declaration and inline implementation inside `namespace main_menu_library` in `src/scene/MainMenuLibrary.h`, before the folder-key declarations:

```cpp
enum class EmptyLibraryBootstrapMode {
  DefaultFolder,
  FolderPicker,
};

constexpr EmptyLibraryBootstrapMode
emptyLibraryBootstrapMode(TargetPlatform platform) noexcept {
  switch (platform) {
  case TargetPlatform::iOS:
  case TargetPlatform::Android:
    return EmptyLibraryBootstrapMode::DefaultFolder;
  case TargetPlatform::Windows:
  case TargetPlatform::MacOS:
  case TargetPlatform::Linux:
    return EmptyLibraryBootstrapMode::FolderPicker;
  }
  return EmptyLibraryBootstrapMode::FolderPicker;
}
```

- [ ] **Step 4: Build and run the focused test to verify the policy passes**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^main_menu_library_tests$'
```

Expected: build succeeds and CTest reports `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 5: Replace the initial platform picker branch with policy-driven bootstrap**

In `MainMenuScene::runLibraryRefreshTask()`, replace the complete `#if TARGET_OS_IOS || TARGET_OS_SIMULATOR` / `#elif TARGET_OS_ANDROID` / `#else` / `#endif` block inside the second `if (entries.empty())` with this code. Preserve the existing task status and `pauseTask()` calls before it.

```cpp
    constexpr auto bootstrapMode =
        main_menu_library::emptyLibraryBootstrapMode(TARGET_PLATFORM);
    if constexpr (bootstrapMode ==
                  main_menu_library::EmptyLibraryBootstrapMode::DefaultFolder) {
      const auto path = ChartRepository::DefaultBmsFolderPath();
      ensureLibraryFolderExists(path);
      if (!taskSession->InsertEntry(path)) {
        throw std::runtime_error("Failed to add default library folder");
      }
      entries = taskSession->SelectEffectiveEntries();
      if (entries.empty()) {
        throw std::runtime_error(
            "Default library folder was not available after insertion");
      }
    } else {
      char *folder_c = tinyfd_selectFolderDialog("Select Folder", nullptr);
      std::string folder;
      if (folder_c == nullptr) {
        std::cerr << "tinyfd_selectFolderDialog error: " << strerror(errno)
                  << std::endl;
        std::cout << "Failed to open folder select dialog.\n";

        while (folder.empty()) {
          if (stopToken.stop_requested()) {
            return;
          }

          std::cout << "Enter bms folder path: ";
          std::cin >> folder;
          if (std::cin.eof() || std::cin.fail()) {
            break;
          }
          if (folder.empty()) {
            continue;
          }

          if (!expandCurrentUserHomeShortcut(folder)) {
            std::cout
                << "Could not expand ~ because no home directory is set.\n";
            folder.clear();
            continue;
          }
          std::ifstream test(folder);
          if (!test) {
            folder.clear();
          }
        }

        if (folder.empty()) {
          return;
        }
      } else {
        folder = folder_c;
      }
      const std::filesystem::path path(folder);
      if (!taskSession->InsertEntry(path)) {
        throw std::runtime_error("Failed to add selected library folder");
      }
      entries = taskSession->SelectEffectiveEntries();
    }
```

This deletes the startup call to `PickIOSFolder()` and makes iOS persist the default path with an empty bookmark. It also makes the existing desktop insertion failure explicit.

- [ ] **Step 6: Verify picker call sites and the focused unit test**

Run:

```bash
rg -n "PickIOSFolder\(" src/scene/MainMenuScene.cpp
cmake --build cmake-build-debug --target main_menu_library_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^main_menu_library_tests$'
```

Expected: `PickIOSFolder(` appears exactly once in `MainMenuScene.cpp`, inside `addIOSFolderEntryFromFiles()`. The focused test passes.

- [ ] **Step 7: Run desktop and iOS build verification**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
scripts/ios_firebase_deploy.sh --build-only
```

Expected: both commands exit 0. The iOS helper performs a build-only compile and does not upload a build.

- [ ] **Step 8: Review the final scope and commit**

Run:

```bash
git diff --check
git diff -- src/scene/MainMenuLibrary.h src/scene/MainMenuScene.cpp tests/main_menu_library_tests.cpp
git status --short
```

Expected: no whitespace errors; the diff contains only the startup policy, its assertions, and the empty-library branch change; no unrelated worktree changes were introduced.

Commit:

```bash
git add src/scene/MainMenuLibrary.h src/scene/MainMenuScene.cpp tests/main_menu_library_tests.cpp
git commit -m "fix: stop automatic iOS startup folder picker [skip ci]"
```

Expected: one commit containing exactly the three planned source/test files.

- [ ] **Step 9: Verify the committed result**

Run:

```bash
git show --stat --oneline --summary HEAD
git status --short
```

Expected: the latest commit is `fix: stop automatic iOS startup folder picker [skip ci]`, its stat lists only the three planned files, and the worktree has no changes introduced by this task.
