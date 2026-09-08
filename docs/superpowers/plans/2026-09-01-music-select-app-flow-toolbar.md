# Music-Select Application Flow and Toolbar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` to implement this plan task-by-task. Execute
> inline in the current checkout; the user prohibited worktrees and subagents.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the Intro/Start/Settings flow, explicit Built-in launch, selected
skin scene, diagnostic no-fallback failures, origin-aware utilities, and the
persisted Font Awesome floating toolbar.

**Architecture:** Startup enters a registered `IntroScene` after starting the
application-owned library service. `MusicSelectLaunch` returns one of three
explicit outcomes: native Built-in, a fully constructed skin session, or
diagnostics. A small return-target value keeps Intro/native/skinned origins
exact while retained scenes are paused. The toolbar is an application-owned
overlay inside `MusicSelectScene`; its state is stored under the application
data root, outside every profile.

**Tech Stack:** C++23, SDL2, existing View/Yoga UI, Font Awesome 6 Free Solid,
VersionedJson/AtomicFile, existing scene manager, CMake/CTest.

**Spec:**
`docs/superpowers/specs/2026-09-01-beatoraja-lua-music-select-design.md`

## Global Constraints

- The selected skin path never activates Built-in after acquisition, setup,
  update, input, or render failure and never clears the stored selection.
- Built-in is the type-5 default only when no type-5 selection is stored.
- Neither successful selector has a route back to Intro. Beatoraja ESC exits
  the application.
- The toolbar exists only in `MusicSelectScene`; hidden means no view and no
  hit target.
- Toolbar actions are exactly Music Player, Tasks, IR Uploads, and Settings,
  plus drag/collapse/expand/hide controls. Do not expose parsing logs,
  add/import folder, refresh/rebuild, or Intro navigation.
- Every visible toolbar control uses the bundled Font Awesome font and exact
  codepoints listed in Task 2; do not substitute text labels or platform icons.
- Library scanning runs in Intro, Settings, error, both selectors, and utility
  scenes and pauses only in gameplay.
- Make each commit a coherent production slice with its focused tests; fold
  trivial corrections into that slice and split a slice before it becomes
  broad enough to obscure review.
- Do not modify parser amalgamation files or run a whole-file formatter.

---

### Task 1: Per-device toolbar state persistence

**Files:**
- Create: `src/ApplicationUiState.h`
- Create: `src/ApplicationUiState.cpp`
- Create: `src/ApplicationUiStateStore.h`
- Create: `src/ApplicationUiStateStore.cpp`
- Create: `tests/application_ui_state_store_tests.cpp`
- Modify: `src/context.h`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
enum class MusicSelectToolbarMode { Expanded, Collapsed, Hidden };

struct MusicSelectToolbarState {
  MusicSelectToolbarMode mode = MusicSelectToolbarMode::Expanded;
  float x = 0.0f;
  float y = 0.0f;
  bool hasPosition = false;
};

struct ApplicationUiState {
  static constexpr int kSchemaVersion = 1;
  MusicSelectToolbarState musicSelectToolbar;
};

class ApplicationUiStateStore {
public:
  static ApplicationUiStateLoadResult Load(const std::filesystem::path &path);
  static bool SaveAtomic(const std::filesystem::path &path,
                         const ApplicationUiState &state,
                         std::string &diagnostic);
};
```

`ApplicationContext` loads
`applicationDataRoot / "application-ui-state.json"` before scenes and exposes
one synchronized save operation. This path must not be below
`profileManager.activePaths().root`.

- [ ] **Step 1: Write failing round-trip and scope tests**

Assert all three modes, authored floats, and `hasPosition` round-trip through a
temporary application root. Assert the context path is invariant across
profile changes. Assert a missing file creates the declared initial expanded
state and that save uses the existing atomic-file contract.

- [ ] **Step 2: Run and observe missing state/store symbols**

Run: `cmake --build cmake-build-debug --target application_ui_state_store_tests -j 6`

Expected: compilation fails because the state/store are absent.

- [ ] **Step 3: Implement the versioned device-level store**

Use the existing `VersionedJson` and `AtomicFile` primitives. Keep this schema
separate from `AppSettings`, profile import/export, profile duplication, and
profile switching. Surface read/write diagnostics to Settings without making
toolbar persistence a profile-initialization failure.

- [ ] **Step 4: Run and commit persistence**

Run: `cmake --build cmake-build-debug --target application_ui_state_store_tests -j 6 && ./cmake-build-debug/application_ui_state_store_tests`

Expected: PASS.

```bash
git add CMakeLists.txt src/CMakeLists.txt src/ApplicationUiState.h src/ApplicationUiState.cpp src/ApplicationUiStateStore.h src/ApplicationUiStateStore.cpp src/context.h tests/application_ui_state_store_tests.cpp
git commit -m "feat: persist music select toolbar state"
```

### Task 2: Font Awesome floating toolbar view

**Files:**
- Modify: `src/view/IconText.h`
- Create: `src/scene/MusicSelectToolbarView.h`
- Create: `src/scene/MusicSelectToolbarView.cpp`
- Create: `tests/music_select_toolbar_view_tests.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace ui_icons {
inline constexpr std::uint32_t kDrag = 0xf58e;
inline constexpr std::uint32_t kMusic = 0xf001;
inline constexpr std::uint32_t kTasks = 0xf0ae;
inline constexpr std::uint32_t kIrUploads = 0xf0ee;
inline constexpr std::uint32_t kSettings = 0xf013;
inline constexpr std::uint32_t kCollapse = 0xf077;
inline constexpr std::uint32_t kExpand = 0xf078;
inline constexpr std::uint32_t kHide = 0xf070;
}

struct MusicSelectToolbarCallbacks {
  std::function<void()> openMusicPlayer;
  std::function<void()> openTasks;
  std::function<void()> openIrUploads;
  std::function<void()> openSettings;
  std::function<void(MusicSelectToolbarState)> persist;
};
```

- [ ] **Step 1: Write failing icon/mode/hit tests**

Inspect actual `TextView` children and assert every icon uses
`ui_icons::kFontAwesomeSolidPath`, `ui_icons::textForCodepoint`, and the exact
codepoint above. Expanded has drag plus six action controls; collapsed has
only drag plus Expand; hidden constructs no toolbar view. Assert no text label
is rendered and none of the four excluded application actions is present.

- [ ] **Step 2: Run and observe missing toolbar failure**

Run: `cmake --build cmake-build-debug --target music_select_toolbar_view_tests -j 6`

Expected: compilation fails because the toolbar is absent.

- [ ] **Step 3: Implement expanded/collapsed/hidden construction**

Build the overlay with the existing button/theme primitives. Clamp placement
only to the current application's visible safe-area behavior; persistence
keeps device coordinates. Rebuild controls on a mode change and save the new
mode immediately. Hidden removes the overlay from the scene rather than using
transparent or disabled hit regions.

- [ ] **Step 4: Implement pointer-first dragging and consumption**

The drag handle captures mouse/touch press, updates position during motion,
persists on release, and consumes its sequence. Buttons consume their click
before skin mouse handling. The tests cover drag in expanded and collapsed
modes, no drag from action icons, and no hit result in hidden mode.

- [ ] **Step 5: Run and commit the toolbar component**

Run: `cmake --build cmake-build-debug --target music_select_toolbar_view_tests -j 6 && ./cmake-build-debug/music_select_toolbar_view_tests`

Expected: PASS.

```bash
git add CMakeLists.txt src/scene/CMakeLists.txt src/scene/MusicSelectToolbarView.h src/scene/MusicSelectToolbarView.cpp src/view/IconText.h tests/music_select_toolbar_view_tests.cpp
git commit -m "feat: add Font Awesome selector toolbar"
```

### Task 3: Shared Tasks surface and utility return targets

**Files:**
- Create: `src/scene/SceneReturnTarget.h`
- Create: `src/scene/SceneReturnTarget.cpp`
- Create: `src/scene/LibraryTasksOverlay.h`
- Create: `src/scene/LibraryTasksOverlay.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/MusicPlayerScene.h`
- Modify: `src/scene/MusicPlayerScene.cpp`
- Modify: `src/scene/IrUploadsScene.h`
- Modify: `src/scene/IrUploadsScene.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Create: `tests/scene_return_target_tests.cpp`
- Create: `tests/library_tasks_overlay_tests.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
struct SceneReturnTarget {
  enum class Kind { Registered, Retained };
  Kind kind = Kind::Registered;
  std::string registeredName;
  Scene *retained = nullptr;
};

bool returnToScene(SceneManager &manager, const SceneReturnTarget &target);
```

`LibraryTasksOverlay` consumes only `ChartLibraryTaskService::Snapshot` and
blocks input beneath itself until closed.

- [ ] **Step 1: Write failing return-table tests**

Assert registered Intro and MainMenu destinations plus a retained paused scene.
Assert returning destroys the utility foreground scene, resumes rather than
reinitializes the retained selector, and never invents a fallback when the
declared target is unavailable.

- [ ] **Step 2: Write failing shared Tasks presentation tests**

Move the exact current task status/progress/history text rules from MainMenu
into literal tests for `LibraryTasksOverlay`. Assert it is read-only: no
add/import, refresh/rebuild, log, or Intro action. Assert modal pointer and key
events do not reach the underlying scene.

- [ ] **Step 3: Run and observe missing shared components**

Run: `cmake --build cmake-build-debug --target scene_return_target_tests library_tasks_overlay_tests -j 6`

Expected: compilation fails because both components are absent.

- [ ] **Step 4: Extract Tasks UI and parameterize utilities**

Replace MainMenu's private Tasks modal data formatting with the shared overlay
without moving any excluded native controls. Give `MusicPlayerScene`,
`IrUploadsScene`, and `SettingsScene` explicit return targets. Their Back/ESC
uses that target. `IrUploadsScene::openIrSettings` carries its original target
through Settings rather than routing to MainMenu.

- [ ] **Step 5: Run utility regressions and commit**

Run: `cmake --build cmake-build-debug --target scene_return_target_tests library_tasks_overlay_tests music_playlist_repository_tests ir_uploads_controller_tests main -j 6 && ./cmake-build-debug/scene_return_target_tests && ./cmake-build-debug/library_tasks_overlay_tests && ./cmake-build-debug/music_playlist_repository_tests && ./cmake-build-debug/ir_uploads_controller_tests`

Expected: PASS; native MainMenu utilities still return to native MainMenu.

```bash
git add CMakeLists.txt src/scene/CMakeLists.txt src/scene/SceneReturnTarget.h src/scene/SceneReturnTarget.cpp src/scene/LibraryTasksOverlay.h src/scene/LibraryTasksOverlay.cpp src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp src/scene/MusicPlayerScene.h src/scene/MusicPlayerScene.cpp src/scene/IrUploadsScene.h src/scene/IrUploadsScene.cpp src/scene/SettingsScene.h src/scene/SettingsSceneLayout.cpp tests/scene_return_target_tests.cpp tests/library_tasks_overlay_tests.cpp
git commit -m "refactor: share selector utility surfaces"
```

### Task 4: Intro, origin-aware Settings, and launch outcomes

**Files:**
- Create: `src/scene/IntroScene.h`
- Create: `src/scene/IntroScene.cpp`
- Create: `src/scene/MusicSelectLaunch.h`
- Create: `src/scene/MusicSelectLaunch.cpp`
- Create: `src/scene/MusicSelectSkinErrorScene.h`
- Create: `src/scene/MusicSelectSkinErrorScene.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneSkins.cpp`
- Create: `tests/music_select_launch_tests.cpp`
- Create: `tests/settings_scene_origin_tests.cpp`
- Create: `tests/intro_scene_tests.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
struct MusicSelectLaunchBuiltIn {};
struct MusicSelectLaunchReady {
  ValidatedSkinActivation activation;
};
struct MusicSelectLaunchFailed {
  std::vector<SkinDiagnostic> diagnostics;
};
using MusicSelectLaunchResult = std::variant<
    MusicSelectLaunchBuiltIn, MusicSelectLaunchReady,
    MusicSelectLaunchFailed>;
```

- [ ] **Step 1: Write failing three-way launch tests**

Assert no stored type-5 selection and explicit Built-in both return BuiltIn.
Assert a valid selected type-5 Lua skin returns Ready. Assert missing entry,
package/configuration failure, Lua failure, wrong configured type, model,
resource, and runtime setup failure return Failed with the actual diagnostic,
preserve selection, and never construct MainMenu.

- [ ] **Step 2: Write failing Intro and Settings-origin tests**

Assert the Intro view has the exact `AsoBMaShow`, Start, and Settings actions.
Assert the approved return table: Intro->Intro, Error->Intro,
MainMenu->same MainMenu, MusicSelect->same retained MusicSelect. Assert Settings
can set Expanded/Collapsed/Hidden in application UI state even when opened
from Intro and persists immediately.

- [ ] **Step 3: Run and observe missing flow failures**

Run: `cmake --build cmake-build-debug --target music_select_launch_tests settings_scene_origin_tests intro_scene_tests -j 6`

Expected: compilation fails because the new scenes/outcomes are absent.

- [ ] **Step 4: Implement Intro and exact launch dispatch**

Start reads the type-5 target selection once and calls the existing lifecycle
with `acquireForSkinType(5, false)`. Dispatch BuiltIn to registered MainMenu,
Ready to a newly owned `MusicSelectScene` in Task 5, and Failed to the
diagnostic scene. Do not catch Failed and continue to MainMenu.

- [ ] **Step 5: Implement diagnostic screen and Settings recovery**

Render all ordered diagnostics delivered by activation/runtime setup. Back and
Settings both ultimately return Intro; Settings allows changing the selected
type-5 item and toolbar state. The error screen does not modify either value.

- [ ] **Step 6: Run and commit the application flow**

Run: `cmake --build cmake-build-debug --target music_select_launch_tests settings_scene_origin_tests intro_scene_tests gameplay_skin_settings_tests -j 6 && ./cmake-build-debug/music_select_launch_tests && ./cmake-build-debug/settings_scene_origin_tests && ./cmake-build-debug/intro_scene_tests && ./cmake-build-debug/gameplay_skin_settings_tests`

Expected: PASS.

```bash
git add CMakeLists.txt src/scene/CMakeLists.txt src/scene/IntroScene.h src/scene/IntroScene.cpp src/scene/MusicSelectLaunch.h src/scene/MusicSelectLaunch.cpp src/scene/MusicSelectSkinErrorScene.h src/scene/MusicSelectSkinErrorScene.cpp src/scene/SettingsScene.h src/scene/SettingsScene.cpp src/scene/SettingsSceneLayout.cpp src/scene/SettingsSceneSkins.cpp tests/music_select_launch_tests.cpp tests/settings_scene_origin_tests.cpp tests/intro_scene_tests.cpp
git commit -m "feat: add intro and selector launch flow"
```

### Task 5: Dedicated skinned MusicSelectScene

**Files:**
- Create: `src/scene/MusicSelectScene.h`
- Create: `src/scene/MusicSelectScene.cpp`
- Create: `tests/music_select_scene_tests.cpp`
- Modify: `src/scene/MusicSelectLaunch.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Owns one `MusicSelectController`, `MusicSelectSideEffectRunner`, and
  `MusicSelectSkinSession` for its complete retained lifetime.
- `pausesBackgroundTasksForPerformance()` remains inherited `false`.

- [ ] **Step 1: Write failing lifecycle/input/render tests**

Assert setup consumes Ready activation exactly once; update publishes one
immutable frame; render calls only the skin session; toolbar gets pointer input
first, then skin mouse, then selector logical input; Tasks blocks underlying
input; onPause retains Lua/controller/session and onResume does not reinitialize
them. Assert ESC requests application exit.

- [ ] **Step 2: Write failure-transition tests before implementation**

Inject setup, update, input, and render failures. Each must stop selector work,
release resources deterministically, preserve selected skin configuration, and
open `MusicSelectSkinErrorScene` with the actual ordered reasons. Assert
MainMenu construction count remains zero.

- [ ] **Step 3: Run and observe missing scene failure**

Run: `cmake --build cmake-build-debug --target music_select_scene_tests -j 6`

Expected: compilation fails because `MusicSelectScene` is absent.

- [ ] **Step 4: Implement scene ownership and utility actions**

Build the controller/session before publishing the scene. Expanded toolbar
buttons open `MusicPlayerScene`, `IrUploadsScene`, or Settings with this scene
as a retained return target and `keepBackground=true`; Tasks opens the shared
overlay in place. Collapse/Expand/Hide update application state. Hidden builds
no view and has no recovery path inside the selector.

- [ ] **Step 5: Implement transactional error handoff**

Convert every structured session/controller failure to the diagnostic model
and defer the scene transition outside the failing render/input callback. Do
not call Built-in launch and do not erase or overwrite selection settings.

- [ ] **Step 6: Run and commit the scene**

Run: `cmake --build cmake-build-debug --target music_select_scene_tests music_select_skin_session_tests music_select_controller_tests music_select_toolbar_view_tests -j 6 && ./cmake-build-debug/music_select_scene_tests && ./cmake-build-debug/music_select_skin_session_tests && ./cmake-build-debug/music_select_controller_tests && ./cmake-build-debug/music_select_toolbar_view_tests`

Expected: PASS.

```bash
git add CMakeLists.txt src/scene/CMakeLists.txt src/scene/MusicSelectScene.h src/scene/MusicSelectScene.cpp src/scene/MusicSelectLaunch.cpp tests/music_select_scene_tests.cpp
git commit -m "feat: run selected music select skins"
```

### Task 6: Startup wiring and shared-service lifetime

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/context.h`
- Modify: `src/scene/SceneManager.cpp`
- Modify: `tests/application_startup_tests.cpp`
- Create: `tests/music_select_application_flow_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing end-to-end scene-flow tests**

Cover startup->Intro; Intro Settings->Intro; Start Built-in->MainMenu; Start
selected->MusicSelect; activation/runtime failure->Error; Error Back->Intro;
Error Settings->Intro; native utilities/settings->same MainMenu; skin
utilities/settings->same live MusicSelect; and absence of selector->Intro
actions. Assert toolbar presence only for the skinned selector.

- [ ] **Step 2: Write the pause-policy integration test**

Start a controllable library task and prove its progress advances in Intro,
Settings, Error, MainMenu, MusicSelect, MusicPlayer, IR Uploads, and Tasks. Move
to `GamePlayScene`, prove its existing checkpoint pauses progress, then leave
gameplay and prove it resumes.

- [ ] **Step 3: Run and observe startup expectation failures**

Run: `cmake --build cmake-build-debug --target application_startup_tests music_select_application_flow_tests -j 6`

Expected: current MainMenu-first assertions fail.

- [ ] **Step 4: Start services once and enter Intro**

After database/result recovery, start the application-owned library service,
register Intro and MainMenu, and enter Intro. Remove the registered Settings
assumption if Settings is now origin-parameterized. Shutdown the service once
after the scene loop; scene transitions do not restart it.

- [ ] **Step 5: Run flow/build regressions and commit wiring**

Run: `cmake --build cmake-build-debug --target application_startup_tests music_select_application_flow_tests main -j 6 && ./cmake-build-debug/application_startup_tests && ./cmake-build-debug/music_select_application_flow_tests`

Expected: PASS.

```bash
git add CMakeLists.txt src/context.h src/main.cpp src/scene/SceneManager.cpp tests/application_startup_tests.cpp tests/music_select_application_flow_tests.cpp
git commit -m "feat: start AsoBMaShow from intro"
```

### Task 7: Specimen acceptance and final verification

**Files:**
- Modify only owning production/test/ledger files when a focused failure proves
  a defect.

- [ ] **Step 1: Run the complete source and evidence gates**

Run:

```bash
python3 scripts/extract_beatoraja_music_select_skin_surface.py \
  --beatoraja-root /Users/xf/workspace/SNURhythm/beatoraja --check
python3 -m unittest tests/beatoraja_music_select_skin_ledger_tests.py \
  tests/beatoraja_music_select_skin_ledger_evidence_tests.py -v
```

Expected: pinned full commit matches, no row is missing, and every implemented
or source-no-op row has executable evidence.

- [ ] **Step 2: Run focused and full desktop verification**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure \
  -R 'music_select|chart_library|application_startup|scene_return|toolbar' -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
cmake --build cmake-build-debug --target main -j 6
git diff --check
```

Expected: every test and build passes; no whitespace errors.

- [ ] **Step 3: Exercise local third-party specimens as acceptance only**

Enumerate `.luaskin` type-5 entries under `/Users/xf/Downloads/Skins`, import
through the normal package/catalog path, and launch each selected specimen.
Record loader/runtime diagnostics verbatim. Fix only behavior proven by the
pinned source contract; never add a specimen-specific extension or validation.

- [ ] **Step 4: Verify no-fallback and persisted toolbar manually**

Exercise Built-in default, successful skin, deliberately broken selected skin,
expanded/collapsed/hidden persistence, drag persistence, utility returns, and
restart->Intro Settings hidden recovery. Confirm no toolbar or route to Intro
appears in native MainMenu and no failed skin opens it.

- [ ] **Step 5: Keep corrections in their owning feature slice**

For any failure, first add or identify its focused failing test, then amend the
smallest owning production/test/ledger slice. Do not make a specimen-only,
verification-only, or broad cleanup commit.

### Task 8: Push, ready-for-review PR, and automatic-review closure

**Files:**
- Modify only files required by verified review findings.

- [ ] **Step 1: Load the finishing and review skills**

Read and follow `superpowers:finishing-a-development-branch`,
`superpowers:requesting-code-review`, and, for every returned finding,
`superpowers:receiving-code-review`. The user's explicit delivery choice is to
push the current branch and open a ready-for-review PR; do not offer a worktree,
merge, or draft-PR alternative.

- [ ] **Step 2: Audit commit size and branch/base facts**

Run `git status --short`, inspect the feature-branch log/diff, and verify each
commit is a coherent feature slice with focused tests. Fold trivial local
follow-ups into the owning unpushed commit; split any genuinely oversized
unpublished slice without rewriting commits that were not made for this task.
Resolve the repository owner, remote, and PR base from Git/GitHub metadata;
do not guess a base branch.

- [ ] **Step 3: Push and create a non-draft PR**

Push the current feature branch to its resolved remote, then use `gh pr create`
with the resolved base, an implementation-focused title/body, the verified
test commands, the pinned Beatoraja revision, and explicit no-fallback and
toolbar behavior. Confirm the resulting PR reports `isDraft: false`.

- [ ] **Step 4: Identify and poll the automatic reviewer**

Poll the PR's issue reactions, reviews, inline comments, and GraphQL review
threads. Record the actor that attaches the expected `eyes` reaction rather
than assuming a bot login. An `eyes` reaction, pending review, empty interval,
or approval review without that actor's `+1` reaction is not completion.

- [ ] **Step 5: Apply every verified finding with tests and reasonable commits**

For each review comment/thread, inspect the cited code and reproduce the
problem. Add or identify a focused failing test, make the source-grounded fix,
run the focused and proportionate regression suite, commit related findings as
reviewable production/test slices, and push. Reply with concrete evidence when
appropriate; never dismiss a thread merely to make it disappear.

- [ ] **Step 6: Repeat until the reviewer changes its reaction to `+1`**

Continue polling after every push. If new or unresolved findings appear,
repeat Step 5. Stop only when the same recorded automatic-review actor has a
`+1` reaction on the PR and no actionable review thread remains. Rerun the
final verification gate after the last code change and report the PR URL,
final commit, tests, and reviewer reaction.
