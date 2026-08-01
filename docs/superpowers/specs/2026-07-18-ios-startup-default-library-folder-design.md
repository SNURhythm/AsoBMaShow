# iOS Startup Default Library Folder Design

**Date:** July 18, 2026  
**Status:** Approved for implementation

## Objective

Stop iOS from presenting a folder picker automatically when AsoBMaShow starts
with an empty chart-library database. Preserve a usable default chart folder
and leave the existing user-initiated **Add Folder** flow unchanged.

## Verified Current Behavior

`MainMenuScene::init()` enqueues a library refresh immediately. In
`MainMenuScene::runLibraryRefreshTask()`, an empty effective-entry list follows
an iOS-only branch that calls `PickIOSFolder()`. This produces the unsolicited
startup `UIDocumentPickerViewController`.

The current iOS cancellation fallback creates and scans
`ChartRepository::DefaultBmsFolderPath()`, but it only appends that path to the
in-memory scan list. It does not persist the default path in the chart-library
`entries` table at that call site.

Default-folder persistence already exists in other flows:

- Settings chart-library rebuild creates and inserts the default path when the
  effective entry list is empty.
- BMS download-root selection can create and insert the default path.
- Android's `SelectEffectiveEntries()` supplies its default folder even when no
  stored entry exists.

The initial iOS refresh is therefore the inconsistent path.

## Startup Behavior

When the effective chart-library entry list is empty:

- iOS uses the application-owned default BMS folder;
- Android continues to use its default-folder behavior; and
- desktop platforms retain their existing folder-picker and console fallback.

The iOS flow will:

1. resolve `ChartRepository::DefaultBmsFolderPath()`;
2. create the directory with the existing guarded directory helper;
3. persist the path with `ChartRepository::Session::InsertEntry()` and an empty
   iOS bookmark;
4. reload the effective entry list; and
5. scan the resulting entry normally.

The bootstrap runs only after the repository reports no effective entries.
Existing user-selected folders and bookmarks are not replaced or modified.

## User-Initiated Folder Selection

`MainMenuScene::addIOSFolderEntryFromFiles()` remains unchanged. The **Add
Folder** button and Settings callback continue to call it, so
`PickIOSFolder()` remains available only after an explicit user action.

The native picker delegate and `UIDocumentPickerViewController` implementation
in `src/iOSNatives.mm` do not change.

## Startup Policy Seam

Add a small platform-to-bootstrap policy in `src/scene/MainMenuLibrary.h` so
the behavior can be tested without presenting native UI. The policy has two
modes:

- `DefaultFolder` for iOS and Android; and
- `FolderPicker` for desktop platforms.

`runLibraryRefreshTask()` selects the current compile target's policy and
follows the corresponding branch. This keeps the platform decision explicit,
prevents an iOS regression back to an automatic picker, and avoids introducing
a native-UI mock.

## Failure Handling

Default-folder creation or database insertion failure must fail the library
refresh task with a specific error. It must not fall back to an automatic
picker. After insertion, an unexpectedly empty effective-entry list is also an
error because continuing would silently produce an unusable library state.

The existing library-task machinery reports the failure. No new modal or
startup prompt is introduced.

## Scope

Files expected to change:

- `src/scene/MainMenuLibrary.h` for the pure startup policy;
- `src/scene/MainMenuScene.cpp` for the empty-library bootstrap behavior; and
- `tests/main_menu_library_tests.cpp` for platform-policy regression coverage.

No changes are required to repository schemas, iOS native picker code, Android
permissions, Settings UI, or the manual Add Folder workflow.

## Verification

1. Add a failing unit assertion that iOS maps to `DefaultFolder`, Android maps
   to `DefaultFolder`, and desktop maps to `FolderPicker`.
2. Run the focused `main_menu_library_tests` target before and after the policy
   implementation.
3. Confirm `runLibraryRefreshTask()` no longer calls `PickIOSFolder()` while
   `addIOSFolderEntryFromFiles()` still does.
4. Run the desktop `main` build required by the repository instructions.
5. Run `scripts/ios_firebase_deploy.sh --build-only` to compile the actual iOS
   target without uploading a build.
6. Review the final diff and ensure only the planned files changed.
