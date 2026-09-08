# Beatoraja Music-Select Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` to implement this plan task-by-task. Execute
> inline in the current checkout; the user prohibited worktrees and subagents.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the complete source-derived selector state, bar hierarchy,
input behavior, properties, timers, events, writers, preview/ranking/replay
integration, and gameplay launch required by Beatoraja type-5 skins.

**Architecture:** `MusicSelectController` is the non-visual authority for one
selector session. It projects AsoBMaShow repositories into source-equivalent
bar values, owns navigation and selection state, publishes immutable
`MusicSelectSkinFrame` snapshots, and accepts actions through the skin bridge.
Small adapters own preview, IR ranking, replay, and gameplay launch so neither
the native nor skinned selector owns duplicated application services.

**Tech Stack:** C++23, SDL2 logical input, `ChartRepository`,
`ScoreRepository`, `ReplayRepository`, existing audio/IR services, CMake/CTest.

**Spec:**
`docs/superpowers/specs/2026-09-01-beatoraja-lua-music-select-design.md`

## Global Constraints

- Compatibility authority is exactly Beatoraja commit
  `c2ed5db1a46145ed10790c3872f717e95b59db9d` at
  `/Users/xf/workspace/SNURhythm/beatoraja`.
- Generate the applicable ledger rows before implementing each task and use
  their file/symbol locations as the acceptance list. Do not implement a
  remembered or inferred selector contract.
- Add no skin admission or action-argument validation absent from the pinned
  source. Existing repository and gameplay invariants remain authoritative at
  their current boundaries.
- Preserve source ordering, sentinel values, wrap behavior, button priority,
  timer transitions, and exact source-defined no-ops.
- Library scanning continues while this controller is active and pauses only
  when the foreground scene is gameplay.
- Make each commit a coherent production slice with its focused tests and
  ledger rows; fold trivial corrections into that slice and split a slice
  before it becomes broad enough to obscure review.
- Do not modify parser amalgamation files or run a whole-file formatter.

---

### Task 1: Value-owned selector model and repository projection

**Files:**
- Modify: `src/music_select/MusicSelectTypes.h`
- Create: `src/music_select/MusicSelectRepositoryProjection.h`
- Create: `src/music_select/MusicSelectRepositoryProjection.cpp`
- Create: `tests/music_select_repository_projection_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: repository records, scores, replay availability, imported tables,
  active profile options, and the source rows for every `select/bar` class.
- Produces value-owned variants for the exact concrete source classes:

```cpp
struct MusicSelectBarId { std::string value; };

struct MusicSelectBar {
  MusicSelectBarId id;
  MusicSelectBarKind kind = MusicSelectBarKind::Song;
  std::string title;
  std::optional<ChartMetaRecord> chart;
  std::vector<MusicSelectBarId> children;
  MusicSelectBarFrame presentation;
  bool selectable = false;
  bool sortable = false;
};

struct MusicSelectProjection {
  std::vector<MusicSelectBar> bars;
  std::vector<MusicSelectBarId> root;
  std::uint64_t repositoryRevision = 0;
};
```

- Every source field used by `BarRenderer`, the property factories, or an
  event receives an explicit value in the projection. No controller stores a
  raw repository row past projection.

- [ ] **Step 1: Generate the exact bar-class rows and write failing tests**

Refresh the source surface, then write literal repository fixtures covering
each concrete bar class. Assert titles, stable identity, selectable/directory
classification, existence, score/rival lamps, replay slots, trophy/rank
inputs, add date, feature labels, and directory aggregate arrays exactly where
the pinned class exposes them.

- [ ] **Step 2: Run and observe the absent projection failure**

Run: `cmake --build cmake-build-debug --target music_select_repository_projection_tests -j 6`

Expected: compilation fails because the value types and projector are absent.

- [ ] **Step 3: Implement source-equivalent value projection**

Map current repository/table/favorite/search/course data to the concrete bar
classes reached from `BarManager.init`, `updateBar`, and `createCommandBar`.
Keep missing chart paths represented as `SongBar.existsSong() == false`; do
not delete those rows during projection. Compute directory lamps/ranks through
the same mode and path conditions used by `DirectoryBar.updateFolderStatus`.

- [ ] **Step 4: Prove immutable ownership and revision replacement**

After projection, mutate and destroy the fixture repositories and assert the
snapshot remains valid. Replace it with a new revision and assert stable IDs
rebind selection without retaining old references.

- [ ] **Step 5: Run and commit the projection slice**

Run: `cmake --build cmake-build-debug --target music_select_repository_projection_tests -j 6 && ./cmake-build-debug/music_select_repository_projection_tests`

Expected: PASS.

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/CMakeLists.txt src/music_select/MusicSelectTypes.h src/music_select/MusicSelectRepositoryProjection.h src/music_select/MusicSelectRepositoryProjection.cpp tests/music_select_repository_projection_tests.cpp
git commit -m "feat: project Beatoraja music select bars"
```

### Task 2: Bar manager navigation, filtering, sorting, and movement

**Files:**
- Create: `src/music_select/MusicSelectBarManager.h`
- Create: `src/music_select/MusicSelectBarManager.cpp`
- Create: `tests/music_select_bar_manager_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `MusicSelectProjection`, source-equivalent selector configuration,
  and monotonic timestamps.
- Produces:

```cpp
struct MusicSelectBarManagerSnapshot {
  std::vector<MusicSelectBar> rows;
  std::size_t selectedIndex = 0;
  std::vector<MusicSelectBarId> directory;
  std::string directoryText;
  int movementDirection = 0;
  std::int64_t movementEndMillis = 0;
};

class MusicSelectBarManager {
public:
  bool openSelected();
  void close();
  void move(bool increase);
  void setSelectedPosition(float value);
  void refresh(MusicSelectProjection projection);
  MusicSelectBarManagerSnapshot snapshot() const;
};
```

- [ ] **Step 1: Write source-literal failing navigation tests**

Translate the decision tables from `BarManager.init`, both `updateBar`
overloads, `close`, `setSelected`, `get/setSelectedPosition`, and `move` into
literal tests. Cover wraparound, prior-row reselection, directory stack text,
mode/difficulty/LN filtering, invisible charts, sortable directories, random
course results, searches, append folders, same-folder membership, and empty
directories.

- [ ] **Step 2: Run and observe the missing manager failure**

Run: `cmake --build cmake-build-debug --target music_select_bar_manager_tests -j 6`

Expected: compilation fails because `MusicSelectBarManager` is absent.

- [ ] **Step 3: Implement the source transition table**

Port only the branches exercised by the pinned manager and concrete bars.
Preserve row order before and after source-defined sort/filter steps, the
directory queue behavior, selected-position float semantics, and movement
timestamps consumed by `BarRenderer`. Refresh swaps value-owned projections
at a single boundary.

- [ ] **Step 4: Mutation-check branch behavior**

Temporarily invert move direction, remove wraparound, and bypass one directory
filter; confirm the corresponding literal tests fail, then restore the source
behavior.

- [ ] **Step 5: Run and commit navigation**

Run: `cmake --build cmake-build-debug --target music_select_bar_manager_tests music_select_repository_projection_tests -j 6 && ./cmake-build-debug/music_select_bar_manager_tests && ./cmake-build-debug/music_select_repository_projection_tests`

Expected: PASS.

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/CMakeLists.txt src/music_select/MusicSelectBarManager.h src/music_select/MusicSelectBarManager.cpp tests/music_select_bar_manager_tests.cpp
git commit -m "feat: navigate Beatoraja music select bars"
```

### Task 3: Exact music-select input processor

**Files:**
- Create: `src/music_select/MusicSelectInputProcessor.h`
- Create: `src/music_select/MusicSelectInputProcessor.cpp`
- Create: `tests/music_select_input_processor_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: one value-owned logical-input snapshot, source scroll durations,
  analog ticks-per-scroll, current controller state, and a monotonic clock.
- Produces an ordered list of source actions; it does not mutate repositories.

```cpp
struct MusicSelectLogicalInput {
  std::vector<bool> keys;
  std::vector<bool> changed;
  std::vector<bool> analog;
  std::vector<int> analogDelta;
  int wheel = 0;
  bool start = false;
  bool select = false;
  std::set<MusicSelectControlKey> controls;
  std::set<MusicSelectCommandKey> commands;
};

struct MusicSelectInputAction {
  MusicSelectInputActionKind kind;
  int arg1 = 0;
  int arg2 = 0;
};
```

- [x] **Step 1: Write exhaustive key-matrix and priority tests**

Copy the three assignment matrices from `MusicSelectKeyProperty` as literal
fixtures. Cover non-analog versus analog reads, reset-state consumption,
start/select/NUM5 panel priority, option-open/close edges, acceleration,
wheel/analog accumulation, duration acceleration after 50 repeats, ordinary
bar input, play/practice/autoplay/replay, folder open/close, all `KeyCommand`
branches, NUM0-9 branches, post-selection timer ordering, and ESC app exit.

- [x] **Step 2: Run and observe the missing processor failure**

Run: `cmake --build cmake-build-debug --target music_select_input_processor_tests -j 6`

Expected: compilation fails because the processor is absent.

- [x] **Step 3: Implement the pinned processor in source order**

Evaluate branches in the same order as `MusicSelectInputProcessor.input` and
emit actions in that order. Route SDL/controller/touch state through the
existing logical input layer before this class. ESC emits `ExitApplication`;
it never emits an Intro or native-selector transition.

- [x] **Step 4: Prove stateful repeat behavior**

Use a fake monotonic clock and consecutive snapshots to assert `duration`,
`angle`, analog remainder, option pressed/released state, and duration-change
counter transitions. No wall-clock sleeps are allowed in the tests.

- [x] **Step 5: Run and commit input behavior**

Run: `cmake --build cmake-build-debug --target music_select_input_processor_tests -j 6 && ./cmake-build-debug/music_select_input_processor_tests`

Expected: PASS.

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/CMakeLists.txt src/music_select/MusicSelectInputProcessor.h src/music_select/MusicSelectInputProcessor.cpp tests/music_select_input_processor_tests.cpp
git commit -m "feat: process Beatoraja music select input"
```

### Task 4: Complete property, timer, and writer projection

**Files:**
- Create: `src/music_select/MusicSelectPropertyProjection.h`
- Create: `src/music_select/MusicSelectPropertyProjection.cpp`
- Create: `tests/music_select_property_projection_tests.cpp`
- Modify: `src/skin/beatoraja/MusicSelectSkinStateBridge.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the full controller snapshot and the exact factory rows selected by
  the extractor from `BooleanPropertyFactory`, `IntegerPropertyFactory`,
  `FloatPropertyFactory`, `StringPropertyFactory`, `TimerPropertyFactory`, and
  `FloatWriter`.
- Produces `MusicSelectPropertyValues` with the pinned ID/name aliases and each
  factory's own absent sentinel.

- [ ] **Step 1: Generate one literal assertion for every factory row**

For every ledger row assigned to this task, add the row ID to the native
runner and assert at least one true/value case plus every distinct source
sentinel branch. Include negative boolean IDs, panel state, selected bar kind,
replay, score/rival/ranking, option/config, directory, selected-position and
ranking-position writer behavior.

- [ ] **Step 2: Run and observe missing projections**

Run: `cmake --build cmake-build-debug --target music_select_property_projection_tests music_select_skin_state_bridge_tests -j 6`

Expected: focused assertions fail for unimplemented selector rows.

- [ ] **Step 3: Implement generated tables over one immutable snapshot**

Each row resolves the same state branches as its pinned lambda/enum constant.
Named and numeric lookups share one table. Do not normalize absent values to
zero and do not infer a value when the selected bar is a different class.

- [ ] **Step 4: Route writers through ordered controller actions**

Implement only the pinned music-select writers. Preserve each writer's clamp,
cast, side effect, or lack thereof exactly; do not add a generic input range
check. Verify writer actions are published only after a successful skin frame.

- [ ] **Step 5: Run evidence gates and commit**

Run: `cmake --build cmake-build-debug --target music_select_property_projection_tests music_select_skin_state_bridge_tests -j 6 && ./cmake-build-debug/music_select_property_projection_tests && ./cmake-build-debug/music_select_skin_state_bridge_tests && python3 -m unittest tests/beatoraja_music_select_skin_ledger_evidence_tests.py -v`

Expected: PASS for every row owned by this task.

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/CMakeLists.txt src/music_select/MusicSelectPropertyProjection.h src/music_select/MusicSelectPropertyProjection.cpp src/skin/beatoraja/MusicSelectSkinStateBridge.cpp tests/music_select_property_projection_tests.cpp
git commit -m "feat: project music select skin properties"
```

### Task 5: Complete source event and command controller

**Files:**
- Create: `src/music_select/MusicSelectController.h`
- Create: `src/music_select/MusicSelectController.cpp`
- Create: `tests/music_select_controller_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: input actions plus staged skin events/string/float writers.
- Produces the next immutable frame and explicit side-effect requests:

```cpp
struct MusicSelectSideEffect {
  MusicSelectSideEffectKind kind;
  std::optional<MusicSelectBarId> bar;
  std::vector<int> arguments;
  std::string text;
};

struct MusicSelectControllerUpdate {
  MusicSelectSkinFrame frame;
  std::vector<MusicSelectSideEffect> sideEffects;
};
```

- [ ] **Step 1: Write a literal test for every `EventFactory` selector branch**

Use the extractor's exact enum/method rows. Cover mode/difficulty/sort/LN,
options and gauge, target movement, play modes/replays, folder operations,
rival, favorites, selected chart update, open document/IR/explorer, search,
same-folder, hash copy, panel/timer changes, chart-replication mode, and every
remaining source branch. Assert the exact source-defined no-op where its
conditions do not match the current bar/state.

- [ ] **Step 2: Run and observe missing controller actions**

Run: `cmake --build cmake-build-debug --target music_select_controller_tests -j 6`

Expected: compilation or evidence assertions fail because the controller is
absent.

- [ ] **Step 3: Implement events in pinned order and conditions**

Dispatch by the exact event IDs/names exported to Lua. Mutate configuration,
bar manager, selected replay, panel, and timers only in the source branches.
Publish filesystem, clipboard, search prompt, audio, ranking, and gameplay
work as typed side effects for adapters; do not execute them inside property
evaluation.

- [ ] **Step 4: Integrate input and skin actions transactionally**

One update processes logical input, applies source state changes, accepts the
previous successful render's staged skin actions in order, refreshes selected
bar state, switches `TIMER_SONGBAR_CHANGE`, then publishes one serial-numbered
frame. A failed skin render publishes none of its staged actions.

- [ ] **Step 5: Run and commit controller behavior**

Run: `cmake --build cmake-build-debug --target music_select_controller_tests music_select_input_processor_tests music_select_bar_manager_tests -j 6 && ./cmake-build-debug/music_select_controller_tests && ./cmake-build-debug/music_select_input_processor_tests && ./cmake-build-debug/music_select_bar_manager_tests`

Expected: PASS.

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/CMakeLists.txt src/music_select/MusicSelectController.h src/music_select/MusicSelectController.cpp tests/music_select_controller_tests.cpp
git commit -m "feat: control Beatoraja music selection"
```

### Task 6: Shared preview, ranking, replay, and gameplay-launch adapters

**Files:**
- Create: `src/music_select/MusicSelectSideEffectRunner.h`
- Create: `src/music_select/MusicSelectSideEffectRunner.cpp`
- Create: `src/scene/ChartGameplayLauncher.h`
- Create: `src/scene/ChartGameplayLauncher.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Create: `tests/music_select_side_effect_runner_tests.cpp`
- Create: `tests/chart_gameplay_launcher_tests.cpp`
- Modify: `docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json`
- Modify: `src/CMakeLists.txt`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: ordered `MusicSelectSideEffect` values and application services.
- Produces asynchronous value-owned completions returned to the controller;
  `ChartGameplayLauncher` consumes the current selected chart/course, play
  mode, replay slot, and existing play-option selection.

- [ ] **Step 1: Write failing adapter and launch parity tests**

Extract the non-UI chart/course/replay launch decisions currently embedded in
`MainMenuScene`. Test play, practice, autoplay, replay slots, random course,
missing-path failure, chart load failure, preview cancellation, IR ranking
request/result generation, folder refresh, clipboard hashes, and platform
reveal/open behavior through injected existing services.

- [ ] **Step 2: Run and observe missing adapters**

Run: `cmake --build cmake-build-debug --target music_select_side_effect_runner_tests chart_gameplay_launcher_tests -j 6`

Expected: compilation fails because both adapters are absent.

- [ ] **Step 3: Extract one gameplay-launch authority**

Move the current parser/resource/options/replay/course transition preparation
from MainMenu into `ChartGameplayLauncher`. Keep MainMenu's UI loading state
and callbacks in MainMenu. Both selectors call the same launcher and therefore
reach the existing gameplay-only library pause policy.

- [ ] **Step 4: Implement source side effects with value-owned completions**

Reuse current preview audio, chart repository refresh, ranking service,
replay repository, clipboard, and platform document/reveal APIs. Cancel or
generation-gate completions on selection/session changes exactly where the
pinned selector changes its pending resource. No adapter keeps pointers into a
controller frame.

- [ ] **Step 5: Run regressions and commit integration**

Run: `cmake --build cmake-build-debug --target music_select_side_effect_runner_tests chart_gameplay_launcher_tests main_menu_library_tests main -j 6 && ./cmake-build-debug/music_select_side_effect_runner_tests && ./cmake-build-debug/chart_gameplay_launcher_tests && ./cmake-build-debug/main_menu_library_tests`

Expected: PASS; native MainMenu launches retain current behavior.

```bash
git add CMakeLists.txt docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json src/CMakeLists.txt src/music_select/MusicSelectSideEffectRunner.h src/music_select/MusicSelectSideEffectRunner.cpp src/scene/CMakeLists.txt src/scene/ChartGameplayLauncher.h src/scene/ChartGameplayLauncher.cpp src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp tests/music_select_side_effect_runner_tests.cpp tests/chart_gameplay_launcher_tests.cpp
git commit -m "refactor: share music select launch services"
```

### Task 7: Runtime evidence closure

**Files:**
- Modify only source/test/ledger files from Tasks 1-6 when a focused failure
  proves a runtime defect.

- [ ] **Step 1: Refresh and inspect all runtime ledger assignments**

Run:

```bash
python3 scripts/extract_beatoraja_music_select_skin_surface.py \
  --beatoraja-root /Users/xf/workspace/SNURhythm/beatoraja --write
python3 scripts/extract_beatoraja_music_select_skin_surface.py \
  --beatoraja-root /Users/xf/workspace/SNURhythm/beatoraja --check
```

Expected: all bar/input/property/timer/event/writer rows are implemented or
carry an exact source-defined no-op classification with executable evidence.

- [ ] **Step 2: Run the runtime/evidence suite**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R 'music_select|chart_gameplay_launcher' -j 6 && python3 -m unittest tests/beatoraja_music_select_skin_ledger_tests.py tests/beatoraja_music_select_skin_ledger_evidence_tests.py -v`

Expected: PASS with zero missing runtime rows.

- [ ] **Step 3: Run desktop compile and diff checks**

Run: `cmake --build cmake-build-debug --target main -j 6 && git diff --check`

Expected: PASS and no whitespace errors.

- [ ] **Step 4: Keep corrections in their owning feature slice**

If verification exposes a defect, return to its focused failing test and amend
the owning coherent production/test/ledger slice. Do not create a verification-
only or ledger-only cleanup commit.
