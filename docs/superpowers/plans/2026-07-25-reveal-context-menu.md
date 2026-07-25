# Reveal Context Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the direct main-menu Reveal action with a reusable two-action context menu that can show the selected chart's exact-folder siblings or run the existing file-manager reveal.

**Architecture:** Add a portal-backed `ContextMenuView` that owns action presentation and dismissal while its caller owns the trigger. Extend `ChartMetaQuery` with an exact stored-folder predicate, then add pure same-folder derivation/query helpers in `MainMenuLibrary` and wire a temporary folder scope into `MainMenuScene`. The temporary scope overrides the sidebar category, resets search and filter criteria while preserving sort, and is cleared by the next sidebar selection.

**Tech Stack:** C++23, SDL2 events, Yoga-backed views, SQLite, CMake/CTest.

## Global Constraints

- Menu labels are exactly `Show Same Folder` and `Reveal File`, in that order.
- Same-folder matching uses exact parent equality; descendants do not match.
- Archive-backed charts match the exact virtual inner parent, so `pack.zip/A/` never includes `pack.zip/B/`.
- Entering the temporary folder scope clears search and chart filters while preserving the current sort criterion and direction.
- The folder scope is not persisted or added to the library sidebar; selecting any sidebar item clears it.
- The originally selected chart stays selected and is scrolled into view after the folder list reload.
- Existing platform-specific reveal behavior and error logging remain unchanged.
- The context menu is a reusable common component and must use `OverlayPortal`.
- Do not change parser amalgamation files, archive scanning, physical-folder scanning, or library-sidebar categories.

---

## File Structure

- `src/view/ContextMenuView.h`: reusable action/state/callback interface and public show/dismiss API.
- `src/view/ContextMenuView.cpp`: themed action rows, portal lifetime, anchored placement, and dismissal event handling.
- `tests/context_menu_view_tests.cpp`: common component behavior and lifetime tests.
- `src/repositories/ChartRepository.h`: exact-folder query field.
- `src/repositories/ChartRepositoryQueries.cpp`: exact-folder SQL predicates and bindings across select/count/index paths.
- `tests/chart_repository_tests.cpp`: exact ordinary and archive-folder repository behavior.
- `src/scene/MainMenuLibrary.h` / `src/scene/MainMenuLibrary.cpp`: pure folder derivation, filter-reset, and temporary-scope query helpers.
- `tests/main_menu_library_tests.cpp`: helper behavior for exact folders, archive paths, reset semantics, and query precedence.
- `src/scene/MainMenuScene.h` / `src/scene/MainMenuScene.cpp`: Reveal trigger, menu callbacks, temporary state, sidebar clearing, and selection-preserving reload.
- `scripts/check_reveal_context_menu_flow.py`: focused integration wiring audit.
- `src/view/CMakeLists.txt` / `CMakeLists.txt`: production source, focused test target, and source-audit registration.

---

### Task 1: Reusable Anchored Context Menu

**Files:**
- Create: `src/view/ContextMenuView.h`
- Create: `src/view/ContextMenuView.cpp`
- Create: `tests/context_menu_view_tests.cpp`
- Modify: `src/view/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `OverlayPortal`, `OverlayAnchor`, `placeAnchoredOverlay`, `Button`, `TextView`, SDL mouse/touch/key events, and existing `ui_theme` providers.
- Produces: `ContextMenuView(OverlayPortal *, Callbacks)`, `show(OverlayAnchor, std::vector<Action>, int)`, `dismiss()`, `setViewportSize(int, int)`, and `isOpen() const`.

- [ ] **Step 1: Add the failing component test and CMake target**

Create `tests/context_menu_view_tests.cpp` with a private-access test seam and these behavioral checks:

```cpp
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "../src/view/ContextMenuView.h"
#include "../src/view/OverlayPortal.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cassert>
#include <string>
#include <vector>

int main() {
  OverlayPortal portal(0, 0, 800, 600);
  std::vector<bool> openChanges;
  std::vector<std::string> selections;
  auto *identity = static_cast<ContextMenuView *>(nullptr);
  {
    ContextMenuView menu(
        &portal,
        {.onOpenChanged = [&](bool open) { openChanges.push_back(open); },
         .onActionSelected = [&](const std::string &id) {
           selections.push_back(id);
         }});
    identity = &menu;
    menu.setViewportSize(800, 600);
    menu.show({.x = 700, .y = 520, .width = 90, .height = 58},
              {{.id = "folder", .label = "Show Same Folder"},
               {.id = "reveal", .label = "Reveal File"},
               {.id = "disabled", .label = "Disabled", .enabled = false}},
              210);
    assert(menu.isOpen());
    assert(portal.isPresented(&menu));
    assert(openChanges == std::vector<bool>{true});
    assert(menu.panel->getX() + menu.panel->getWidth() <= 790);
    assert(menu.panel->getY() < 520);

    menu.dispatchAction("disabled");
    assert(selections.empty());
    assert(menu.isOpen());

    menu.dispatchAction("folder");
    assert(selections == std::vector<std::string>{"folder"});
    assert(!menu.isOpen());
    assert(!portal.isPresented(&menu));

    menu.show({.x = 100, .y = 100, .width = 90, .height = 58},
              {{.id = "reveal", .label = "Reveal File"}}, 210);
    SDL_Event escape{};
    escape.type = SDL_KEYDOWN;
    escape.key.repeat = 0;
    escape.key.keysym.sym = SDLK_ESCAPE;
    assert(!menu.handleEventsImpl(escape));
    assert(!menu.isOpen());

    menu.show({.x = 100, .y = 100, .width = 90, .height = 58},
              {{.id = "reveal", .label = "Reveal File"}}, 210);
    SDL_Event back{};
    back.type = SDL_KEYDOWN;
    back.key.repeat = 0;
    back.key.keysym.sym = SDLK_AC_BACK;
    assert(!menu.handleEventsImpl(back));
    assert(!menu.isOpen());

    menu.show({.x = 100, .y = 100, .width = 90, .height = 58},
              {{.id = "reveal", .label = "Reveal File"}}, 210);
    menu.handlePointerDown(120.0F, 120.0F);
    assert(!menu.isOpen());

    menu.show({.x = 100, .y = 100, .width = 90, .height = 58},
              {{.id = "reveal", .label = "Reveal File"}}, 210);
    menu.handlePointerDown(20.0F, 20.0F);
    assert(!menu.isOpen());
  }
  assert(!portal.isPresented(identity));
  assert(openChanges.back() == false);
}
```

Register `context_menu_view_tests` in `CMakeLists.txt` with `ContextMenuView.cpp`, `Button.cpp`, `TextView.cpp`, `View.cpp`, rendering color/common/uniform sources, `${COMMON_LIBS}`, `bgfx`, and `yogacore`; add it to the `asobmashow_register_test` target list. Add `ContextMenuView.cpp` to `src/view/CMakeLists.txt`.

- [ ] **Step 2: Build the test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target context_menu_view_tests -j 6
```

Expected: FAIL because `ContextMenuView.h` and its implementation do not exist.

- [ ] **Step 3: Implement the common component**

Declare this public API in `src/view/ContextMenuView.h`:

```cpp
class ContextMenuView final : public View {
public:
  struct Action {
    std::string id;
    std::string label;
    bool enabled = true;
  };
  struct Callbacks {
    std::function<void(bool)> onOpenChanged;
    std::function<void(const std::string &)> onActionSelected;
  };

  ContextMenuView(OverlayPortal *portal, Callbacks callbacks);
  ~ContextMenuView() override;

  void show(OverlayAnchor anchor, std::vector<Action> actions,
            int menuWidth = 210);
  void dismiss();
  void setViewportSize(int width, int height);
  [[nodiscard]] bool isOpen() const noexcept { return open; }

private:
  OverlayPortal *portal = nullptr;
  Callbacks callbacks;
  OverlayAnchor anchor;
  std::vector<Action> actions;
  View *panel = nullptr;
  int viewportWidth = 0;
  int viewportHeight = 0;
  int requestedMenuWidth = 210;
  bool open = false;

  void rebuildActions();
  void updatePlacement();
  void dispatchAction(const std::string &id);
  void handlePointerDown(float x, float y);
  [[nodiscard]] bool pointInsideAnchor(float x, float y) const;
  [[nodiscard]] bool pointInsidePanel(float x, float y) const;
  bool handleEventsImpl(SDL_Event &event) override;
};
```

Implement these rules in `ContextMenuView.cpp`:

- The root is sized to the viewport; `panel` is an absolute-positioned child.
- The panel uses `panelStrong`, `hairlineStrong`, `controlRadius()`, a one-pixel border, and `kPanelShadow`.
- Each 44-pixel action row uses `control`, `controlHover`, and `controlPressed`, left-aligned 17-pixel text, 14-pixel horizontal padding, and `Button::setEnabled`.
- `show` stores the anchor/actions, rebuilds rows, updates placement, presents `this` through the portal, changes `open` to true, and invokes `onOpenChanged(true)` once.
- `dispatchAction` ignores missing/disabled IDs; otherwise it invokes `onActionSelected(id)` and then `dismiss()`.
- `dismiss` unregisters `this`, changes `open` to false, and invokes `onOpenChanged(false)` once.
- The destructor unregisters from the portal without firing callbacks.
- Pointer down outside the panel dismisses and returns `false`; a pointer down inside the trigger anchor also dismisses and returns `false`, implementing second-press toggle without click-through.
- `handlePointerDown` contains that coordinate-only decision so mouse and touch event conversion share one tested path.
- Escape and Android Back with `repeat == 0` dismiss and return `false`.
- While open, mouse, touch, wheel, and keyboard input not handled by action-row children returns `false` so underlying scene controls cannot activate.
- Window and lifecycle events return `true`.

- [ ] **Step 4: Run the component test to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target context_menu_view_tests -j 6
./cmake-build-debug/context_menu_view_tests
```

Expected: build succeeds and the test exits 0.

- [ ] **Step 5: Commit the component**

```bash
git add CMakeLists.txt src/view/CMakeLists.txt src/view/ContextMenuView.h src/view/ContextMenuView.cpp tests/context_menu_view_tests.cpp
git commit -m "feat: add reusable context menu view"
```

---

### Task 2: Exact-Folder Repository Query

**Files:**
- Modify: `src/repositories/ChartRepository.h:43-71`
- Modify: `src/repositories/ChartRepositoryQueries.cpp:450-1028,1434-1910`
- Modify: `tests/chart_repository_tests.cpp:475-610,940-970`

**Interfaces:**
- Consumes: `chart_storage_identity::StoredPathText`, existing chart filter builders/binders, and all `ChartRepository::Session` query methods.
- Produces: `ChartMetaQuery::exactFolder`, applied consistently by `QueryChartMeta`, `CountChartMeta`, and `FindChartMetaIndex`.

- [ ] **Step 1: Write the failing exact-folder repository test**

Add `testExactFolderQuery()` to `tests/chart_repository_tests.cpp`. Seed a fresh chart database with these rows and unique hashes:

```sql
INSERT INTO chart_meta(path,md5,sha256,title,subtitle,genre,artist,
sub_artist,folder,level,source_priority,source_archive_size) VALUES
('library/A/one.bms','md5-one','sha-one','One','','','','','library/A',1,0,0),
('library/A/two.bms','md5-two','sha-two','Two','','','','','library/A',2,0,0),
('library/A/nested/three.bms','md5-three','sha-three','Three','','','','','library/A/nested',3,0,0),
('library/B/four.bms','md5-four','sha-four','Four','','','','','library/B',4,0,0),
('packs/pack.zip/A/five.bms','md5-five','sha-five','Five','','','','','packs/pack.zip/A',5,0,0),
('packs/pack.zip/A/six.bms','md5-six','sha-six','Six','','','','','packs/pack.zip/A',6,0,0),
('packs/pack.zip/B/seven.bms','md5-seven','sha-seven','Seven','','','','','packs/pack.zip/B',7,0,0);
```

Assert the following real session behavior:

```cpp
const auto queryPaths = [](ChartRepository::Session &session,
                           const ChartMetaQuery &query) {
  std::vector<ChartMetaRecord> records;
  session.QueryChartMeta(query, records);
  std::vector<std::string> paths;
  paths.reserve(records.size());
  for (const auto &record : records) {
    paths.push_back(
        chart_storage_identity::StoredPathText(record.meta.BmsPath));
  }
  return paths;
};

ChartMetaQuery query;
query.exactFolder = std::filesystem::path("library/A");
assert(queryPaths(*session, query) ==
       std::vector<std::string>({"library/A/one.bms", "library/A/two.bms"}));
assert(session->CountChartMeta(query) == 2);
assert(session->FindChartMetaIndex(query, "library/A/one.bms") == 0);
assert(session->FindChartMetaIndex(query, "library/A/two.bms") == 1);
assert(session->FindChartMetaIndex(query, "library/A/nested/three.bms") == -1);

query.limit = 1;
query.offset = 1;
assert(queryPaths(*session, query) ==
       std::vector<std::string>({"library/A/two.bms"}));
assert(session->CountChartMeta(query) == 2);

query = {};
query.exactFolder = std::filesystem::path("packs/pack.zip/A");
assert(queryPaths(*session, query) ==
       std::vector<std::string>({"packs/pack.zip/A/five.bms",
                                 "packs/pack.zip/A/six.bms"}));
assert(session->FindChartMetaIndex(query,
                                   "packs/pack.zip/B/seven.bms") == -1);

query.sortCriterion = ChartRecordSortCriterion::Title;
query.sortDirection = ChartRecordSortDirection::Descending;
assert(queryPaths(*session, query) ==
       std::vector<std::string>({"packs/pack.zip/A/six.bms",
                                 "packs/pack.zip/A/five.bms"}));
assert(session->FindChartMetaIndex(query,
                                   "packs/pack.zip/A/five.bms") == 1);
```

Call `testExactFolderQuery()` from the test executable's `main()`.

- [ ] **Step 2: Run the repository test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target chart_repository_tests -j 6
```

Expected: FAIL because `ChartMetaQuery` has no `exactFolder` member.

- [ ] **Step 3: Implement exact-folder filtering**

Add this field to `ChartMetaQuery` before sort fields:

```cpp
std::optional<std::filesystem::path> exactFolder;
```

In `ChartRepositoryQueries.cpp`, add helpers with one canonical storage conversion:

```cpp
void appendExactFolderFilter(std::string &query, const std::string &alias,
                             const ChartMetaQuery &chartQuery) {
  if (chartQuery.exactFolder.has_value()) {
    query += " AND " + alias + ".folder = @exact_folder";
  }
}

void bindExactFolderFilter(sqlite3_stmt *stmt, int &bindIndex,
                           const ChartMetaQuery &chartQuery) {
  if (chartQuery.exactFolder.has_value()) {
    bindSqliteText(stmt, bindIndex++,
                   chart_storage_identity::StoredPathText(
                       *chartQuery.exactFolder));
  }
}
```

Apply and bind the predicate in matching order in `appendChartMetaFilters` / `bindChartMetaFilterParameters`, `appendDifficultyEntryFilters` / `bindDifficultyEntryFilterParameters`, and `appendDifficultyCourseEntryFilters` / `bindDifficultyCourseEntryFilterParameters`. Include `exactFolder.has_value()` in both chart-join requirement helpers so table/course count paths have `cm.folder` available. The existing select, count, default index CTE, and non-default index scan must then share the same predicate through those common helpers.

- [ ] **Step 4: Run repository tests to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target chart_repository_tests -j 6
./cmake-build-debug/chart_repository_tests
```

Expected: build succeeds and all repository assertions pass.

- [ ] **Step 5: Commit the repository behavior**

```bash
git add src/repositories/ChartRepository.h src/repositories/ChartRepositoryQueries.cpp tests/chart_repository_tests.cpp
git commit -m "feat: query charts by exact folder"
```

---

### Task 3: Same-Folder Scope Helpers

**Files:**
- Modify: `src/scene/MainMenuLibrary.h`
- Modify: `src/scene/MainMenuLibrary.cpp`
- Modify: `tests/main_menu_library_tests.cpp`

**Interfaces:**
- Consumes: `ChartMetaRecord`, `ChartRecordFilters`, `ChartMetaQuery`, and `ChartRecordSortState`.
- Produces: `sameFolderForChart(const ChartMetaRecord &)`, `filtersForSameFolder(const ChartRecordFilters &)`, and `chartQueryForSameFolder(const std::filesystem::path &, const std::string &, const ChartRecordFilters &, int)`.

- [ ] **Step 1: Write failing helper tests**

Add these cases near the start of `tests/main_menu_library_tests.cpp::main()`:

```cpp
ChartMetaRecord explicitFolder;
explicitFolder.meta.BmsPath = "/library/A/song.bms";
explicitFolder.meta.Folder = "/library/A/../A";
ASSERT_EQ(std::filesystem::path("/library/A"),
          *main_menu_library::sameFolderForChart(explicitFolder),
          "stored folder is normalized");

ChartMetaRecord archiveChart;
archiveChart.meta.BmsPath = "/packs/pack.zip/A/song.bms";
ASSERT_EQ(std::filesystem::path("/packs/pack.zip/A"),
          *main_menu_library::sameFolderForChart(archiveChart),
          "archive fallback uses exact virtual parent");

ChartMetaRecord noFolder;
ASSERT_EQ(false, main_menu_library::sameFolderForChart(noFolder).has_value(),
          "empty chart has no same-folder scope");

ChartRecordFilters activeFilters;
activeFilters.clearMarkRank = 5;
activeFilters.scoreRank = "AAA";
activeFilters.bpmMin = 120.0;
activeFilters.bpmMax = 180.0;
activeFilters.difficultyMinLevel = "10";
activeFilters.difficultyMaxLevel = "12";
activeFilters.sort = {.criterion = ChartRecordSortCriterion::Title,
                      .direction = ChartRecordSortDirection::Ascending};
const ChartRecordFilters cleared =
    main_menu_library::filtersForSameFolder(activeFilters);
ASSERT_EQ(false, cleared.clearMarkRank.has_value(), "clear mark reset");
ASSERT_EQ(false, cleared.scoreRank.has_value(), "score rank reset");
ASSERT_EQ(false, cleared.bpmMin.has_value(), "minimum BPM reset");
ASSERT_EQ(false, cleared.bpmMax.has_value(), "maximum BPM reset");
ASSERT_EQ(false, cleared.difficultyMinLevel.has_value(),
          "minimum difficulty reset");
ASSERT_EQ(false, cleared.difficultyMaxLevel.has_value(),
          "maximum difficulty reset");
ASSERT_EQ(static_cast<int>(ChartRecordSortCriterion::Title),
          static_cast<int>(cleared.sort.criterion), "sort retained");
ASSERT_EQ(static_cast<int>(ChartRecordSortDirection::Ascending),
          static_cast<int>(cleared.sort.direction), "sort direction retained");

const ChartMetaQuery folderQuery = main_menu_library::chartQueryForSameFolder(
    "/packs/pack.zip/A", "", cleared, 2);
ASSERT_EQ(std::filesystem::path("/packs/pack.zip/A"),
          *folderQuery.exactFolder, "query uses exact folder");
ASSERT_EQ(0, folderQuery.tableId, "sidebar table scope is absent");
ASSERT_EQ(false, folderQuery.favoritesOnly,
          "sidebar favorite scope is absent");
ASSERT_EQ(2, folderQuery.selectedLongNoteMode, "LN mode is retained");
ASSERT_EQ(static_cast<int>(ChartRecordSortCriterion::Title),
          static_cast<int>(folderQuery.sortCriterion),
          "folder query retains sort");
```

- [ ] **Step 2: Run the helper test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests -j 6
```

Expected: FAIL because all three same-folder helpers are undeclared.

- [ ] **Step 3: Implement the pure helpers**

Expose these declarations in `MainMenuLibrary.h` and include `ChartRecordFilters.h`:

```cpp
std::optional<std::filesystem::path>
sameFolderForChart(const ChartMetaRecord &record);

ChartRecordFilters
filtersForSameFolder(const ChartRecordFilters &current);

ChartMetaQuery chartQueryForSameFolder(
    const std::filesystem::path &folder, const std::string &keyword,
    const ChartRecordFilters &filters, int selectedLongNoteMode);
```

Implement them in `MainMenuLibrary.cpp` as follows:

```cpp
std::optional<std::filesystem::path>
sameFolderForChart(const ChartMetaRecord &record) {
  std::filesystem::path folder = record.meta.Folder;
  if (folder.empty() && !record.meta.BmsPath.empty()) {
    folder = record.meta.BmsPath.parent_path();
  }
  if (folder.empty()) {
    return std::nullopt;
  }
  return folder.lexically_normal();
}

ChartRecordFilters
filtersForSameFolder(const ChartRecordFilters &current) {
  ChartRecordFilters reset;
  reset.sort = current.sort;
  return reset;
}

ChartMetaQuery chartQueryForSameFolder(
    const std::filesystem::path &folder, const std::string &keyword,
    const ChartRecordFilters &filters, int selectedLongNoteMode) {
  ChartMetaQuery query;
  query.exactFolder = folder.lexically_normal();
  query.keyword = keyword;
  query.selectedLongNoteMode = selectedLongNoteMode;
  chart_record_filters::applyToQuery(query, filters, false);
  query.sortCriterion = filters.sort.criterion;
  query.sortDirection = filters.sort.direction;
  return query;
}
```

The explicit sort reassignment preserves a pre-existing difficulty sort even though table-specific difficulty-range filters are disabled in a physical-folder scope.

- [ ] **Step 4: Run helper and repository tests to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target main_menu_library_tests chart_repository_tests -j 6
./cmake-build-debug/main_menu_library_tests
./cmake-build-debug/chart_repository_tests
```

Expected: both executables exit 0.

- [ ] **Step 5: Commit the helpers**

```bash
git add src/scene/MainMenuLibrary.h src/scene/MainMenuLibrary.cpp tests/main_menu_library_tests.cpp
git commit -m "feat: model temporary same-folder chart scope"
```

---

### Task 4: Wire the Reveal Menu into Main Menu

**Files:**
- Modify: `src/scene/MainMenuScene.h:18-40,243-270,488-502,698-710`
- Modify: `src/scene/MainMenuScene.cpp:1260-1270,2860-2870,3386-3398,3696-3790,3792-3837,3885-3960,4180-4199,4528-4559,5533-5554,10560-10625`
- Create: `scripts/check_reveal_context_menu_flow.py`
- Modify: `CMakeLists.txt:480-540`

**Interfaces:**
- Consumes: `ContextMenuView`, the three `main_menu_library` helpers, `ChartMetaQuery::exactFolder`, `TextInputBox::setEditingText`, and `reloadChartList(true)` selection restoration.
- Produces: `toggleRevealContextMenu()`, `showSelectedChartFolder()`, `clearSameFolderScope()`, `revealButton`, `revealContextMenu`, and `temporaryChartFolder` scene state.

- [ ] **Step 1: Add a failing main-menu wiring audit**

Create `scripts/check_reveal_context_menu_flow.py`:

```python
#!/usr/bin/env python3
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
header = (root / "src/scene/MainMenuScene.h").read_text()
source = (root / "src/scene/MainMenuScene.cpp").read_text()

checks = {
    "reveal button member": "Button *revealButton" in header,
    "temporary folder state": "temporaryChartFolder" in header,
    "context menu ownership": "std::unique_ptr<ContextMenuView> revealContextMenu" in header,
    "toggle method": "toggleRevealContextMenu" in source,
    "show-folder action": '"Show Same Folder"' in source,
    "reveal-file action": '"Reveal File"' in source,
    "folder query precedence": "chartQueryForSameFolder" in source,
    "selection preserving reload": re.search(
        r"showSelectedChartFolder[\\s\\S]*?reloadChartList\\(true\\)", source
    ) is not None,
    "sidebar clears scope": re.search(
        r"selectFolder[\\s\\S]*?clearSameFolderScope", source
    ) is not None,
    "cleanup releases menu": "revealContextMenu.reset()" in source,
}

missing = [name for name, present in checks.items() if not present]
if missing:
    raise SystemExit("missing Reveal context-menu wiring: " + ", ".join(missing))
```

Register it in the existing test block:

```cmake
add_test(NAME reveal_context_menu_flow_audit
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/check_reveal_context_menu_flow.py
            ${CMAKE_SOURCE_DIR})
set_tests_properties(reveal_context_menu_flow_audit PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
```

- [ ] **Step 2: Run the audit to verify RED**

Run:

```bash
python3 scripts/check_reveal_context_menu_flow.py .
```

Expected: FAIL listing the missing scene state and wiring.

- [ ] **Step 3: Add scene state and build the Reveal menu**

In `MainMenuScene.h`, forward-declare `ContextMenuView` and add:

```cpp
Button *revealButton = nullptr;
std::unique_ptr<ContextMenuView> revealContextMenu;
std::optional<std::filesystem::path> temporaryChartFolder;

void toggleRevealContextMenu();
void showSelectedChartFolder();
bool clearSameFolderScope();
```

In `initView`, replace the local Reveal button and direct callback with the member button, then create the menu after `overlayPortal` exists:

```cpp
revealContextMenu = std::make_unique<ContextMenuView>(
    overlayPortal,
    ContextMenuView::Callbacks{
        .onOpenChanged = [](bool) {},
        .onActionSelected = [this](const std::string &id) {
          if (id == "show-same-folder") {
            showSelectedChartFolder();
          } else if (id == "reveal-file") {
            revealSelectedChartInFileManager();
          }
        },
    });
revealButton->setOnClickListener([this]() { toggleRevealContextMenu(); });
```

`toggleRevealContextMenu()` must retain the existing transition/selection guards, dismiss when already open, and otherwise call:

```cpp
revealContextMenu->setViewportSize(rendering::window_width,
                                   rendering::window_height);
revealContextMenu->show(
    {.x = revealButton->getX(),
     .y = revealButton->getY(),
     .width = revealButton->getWidth(),
     .height = revealButton->getHeight()},
    {{.id = "show-same-folder", .label = "Show Same Folder"},
     {.id = "reveal-file", .label = "Reveal File"}},
    210);
```

- [ ] **Step 4: Activate and clear temporary folder scope**

Implement `showSelectedChartFolder()` with this exact state transition:

```cpp
void MainMenuScene::showSelectedChartFolder() {
  const auto record = selectedRecordSnapshot();
  if (!record.has_value() || record->unavailable || record->solidArchive ||
      record->meta.BmsPath.empty()) {
    return;
  }
  const auto folder = main_menu_library::sameFolderForChart(*record);
  if (!folder.has_value()) {
    return;
  }

  temporaryChartFolder = *folder;
  searchText.clear();
  searchBox->setEditingText("");
  chartRecordFilters =
      main_menu_library::filtersForSameFolder(chartRecordFilters);
  chartBpmMinText.clear();
  chartBpmMaxText.clear();
  chartClearMarkDropdownOpen = false;
  chartScoreRankDropdownOpen = false;
  chartDifficultyMinDropdownOpen = false;
  chartDifficultyMaxDropdownOpen = false;
  chartDifficultyRangeTableId.reset();

  if (folderRecyclerView->selectedIndex >= 0) {
    if (auto *selected = folderRecyclerView->getViewByIndex(
            folderRecyclerView->selectedIndex)) {
      selected->onUnselected();
    }
  }
  folderRecyclerView->selectedIndex = -1;
  refreshChartFilterPanel();
  reloadChartList(true);
}
```

Implement `clearSameFolderScope()` to reset the optional and return whether a scope was active. Call it at the beginning of `selectFolder`; for expandable items and `CoursesRoot`, a cleared scope must force `reloadChartList()` even when the active sidebar key was otherwise unchanged.

While `temporaryChartFolder` is active, `reloadFolderItems` must set `selectedIndex` to `-1` and skip `onSelected()` for the active sidebar row. This preserves the temporary, unlisted nature of the scope across library refreshes.

- [ ] **Step 5: Give the temporary scope query precedence**

At the top of `chartQueryForActiveFolder()`, before the sidebar switch, add:

```cpp
const int selectedLongNoteMode =
    long_note_mode::valueFromId(profileSelections.longNoteMode);
if (temporaryChartFolder.has_value()) {
  return main_menu_library::chartQueryForSameFolder(
      *temporaryChartFolder, searchText, chartRecordFilters,
      selectedLongNoteMode);
}
```

Use `selectedLongNoteMode` for the existing query path too. In `reloadChartList`, only synthesize a course-start leading row when `temporaryChartFolder` is not active.

In `refreshChartFilterPanel`, treat same-folder scope as having no sidebar clear-mark fallback and no table-specific difficulty range. Do not reset a retained difficulty sort while same-folder scope is active, and pass `difficultySortEnabled = true` to the sort panel for that scope so the retained value stays usable.

- [ ] **Step 6: Complete lifecycle handling**

- Call `revealContextMenu->dismiss()` from `onPause()`.
- Resize it alongside other overlay roots when the rendering viewport changes.
- Reset `revealContextMenu` at the start of `cleanupScene()`, before `overlayPortal` is destroyed or nulled.
- Clear `temporaryChartFolder` during scene cleanup initialization resets.
- Set `revealButton = nullptr` with the other view pointers.
- Keep `revealSelectedChartInFileManager()` unchanged except for its new call site.

- [ ] **Step 7: Run focused tests and the desktop build**

Run:

```bash
python3 scripts/check_reveal_context_menu_flow.py .
cmake --build cmake-build-debug --target context_menu_view_tests chart_repository_tests main_menu_library_tests main -j 6
./cmake-build-debug/context_menu_view_tests
./cmake-build-debug/chart_repository_tests
./cmake-build-debug/main_menu_library_tests
ctest --test-dir cmake-build-debug --output-on-failure -R 'context_menu_view_tests|chart_repository_tests|main_menu_library_tests|reveal_context_menu_flow_audit'
git diff --check
```

Expected: the audit passes, all three executables exit 0, focused CTest reports 100% pass, `main` links successfully, and `git diff --check` prints nothing.

- [ ] **Step 8: Review the final diff against the approved spec**

Confirm all of the following in the diff:

- the two labels and order are exact;
- file-manager reveal code is not behaviorally changed;
- same-folder entry clears criteria but retains sort;
- exact archive parents do not broaden to same-depth siblings;
- selecting every sidebar item path clears the temporary scope;
- menu destruction unregisters from the portal before portal lifetime ends;
- no unrelated parser, scanner, or sidebar-category edits are present.

- [ ] **Step 9: Commit the integration**

```bash
git add CMakeLists.txt scripts/check_reveal_context_menu_flow.py src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp
git commit -m "feat: add Reveal action menu"
```
