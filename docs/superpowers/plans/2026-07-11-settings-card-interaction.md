# Settings Card Interaction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Isolate profile actions inside their owning cards and show binding conflicts in an immediately visible blocking modal.

**Architecture:** Add a small persistent state object for one UUID-bound inline profile editor, then rewire Profile-tab composition around it without changing ProfileSettingsController. Move binding-conflict composition from scroll content to a root-level BlockingOverlayView, retaining InputCaptureController as the transactional source of truth.

**Tech Stack:** C++23, SDL2 events, custom View/Yoga UI, CMake/CTest, Xcode filesystem-synchronized source membership.

## Global Constraints

- Work on feature/player-foundations-local; do not merge or push.
- The top Player Profiles card owns only the new-profile name draft and Create action.
- Profile Rename/Duplicate editing and delete/overwrite confirmation remain UUID-bound.
- The binding-conflict overlay preserves scroll position and blocks mouse, touch, wheel, keyboard, and text-entry events.
- Do not change profile/input persistence formats or controller mutation semantics.
- Add every new src path to the iOS target membershipExceptions.
- Implement each behavior test-first and preserve existing user changes.

---

### Task 1: UUID-bound inline profile-card editor

**Files:**
- Create: src/scene/SettingsSceneProfileEditorState.h
- Modify: src/scene/SettingsScene.h
- Modify: src/scene/SettingsSceneProfiles.cpp
- Modify: src/scene/SettingsSceneLayout.cpp
- Modify: src/scene/SettingsScene.cpp
- Modify: tests/view_layout_tests.cpp
- Modify: ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj

**Interfaces:**
- Consumes: ProfileSettingsController rename, duplicate, and cancelConfirmation; PlayerProfile id and displayName.
- Produces: settings_scene::ProfileInlineEditorState with beginRename, beginDuplicate, updateDraft, requestFor, clear, and clearIfTargetUnavailable. SettingsScene owns one instance across layout rebuilds.

- [ ] **Step 1: Write the failing profile-editor state test**

Add the include and focused test to tests/view_layout_tests.cpp:

~~~~cpp
#include "scene/SettingsSceneProfileEditorState.h"

void testProfileInlineEditorStaysBoundToItsCard() {
  settings_scene::ProfileInlineEditorState editor;
  editor.beginRename("alpha", "Alpha");
  editor.updateDraft("Renamed Alpha");

  assert(!editor.requestFor("bravo").has_value());
  const auto rename = editor.requestFor("alpha");
  assert(rename.has_value());
  assert(rename->action == settings_scene::ProfileInlineEditAction::Rename);
  assert(rename->profileId == "alpha");
  assert(rename->name == "Renamed Alpha");

  editor.beginDuplicate("bravo", "Bravo");
  const auto duplicate = editor.requestFor("bravo");
  assert(duplicate.has_value());
  assert(duplicate->action ==
         settings_scene::ProfileInlineEditAction::Duplicate);
  assert(duplicate->name == "Bravo Copy");

  editor.clearIfTargetUnavailable(true);
  assert(editor.activeFor("bravo"));
  editor.clearIfTargetUnavailable(false);
  assert(!editor.active());
}
~~~~

Call testProfileInlineEditorStaysBoundToItsCard from main.

- [ ] **Step 2: Run the focused test to verify RED**

Run:

~~~~bash
cmake --build cmake-build-debug --target view_layout_tests -j 6
~~~~

Expected: compilation fails because SettingsSceneProfileEditorState.h does not exist.

- [ ] **Step 3: Add the minimal persistent editor state**

Create src/scene/SettingsSceneProfileEditorState.h:

~~~~cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace settings_scene {

enum class ProfileInlineEditAction { Rename, Duplicate };

struct ProfileInlineEditRequest {
  ProfileInlineEditAction action = ProfileInlineEditAction::Rename;
  std::string profileId;
  std::string name;
};

class ProfileInlineEditorState {
public:
  void beginRename(std::string_view profileId, std::string_view currentName) {
    begin(ProfileInlineEditAction::Rename, profileId, currentName);
  }

  void beginDuplicate(std::string_view profileId,
                      std::string_view currentName) {
    begin(ProfileInlineEditAction::Duplicate, profileId,
          std::string(currentName) + " Copy");
  }

  [[nodiscard]] bool active() const { return action_.has_value(); }
  [[nodiscard]] bool activeFor(std::string_view profileId) const {
    return active() && profileId_ == profileId;
  }
  [[nodiscard]] std::string_view draft() const { return draft_; }

  void updateDraft(std::string draft) {
    if (active()) {
      draft_ = std::move(draft);
    }
  }

  [[nodiscard]] std::optional<ProfileInlineEditRequest>
  requestFor(std::string_view profileId) const {
    if (!activeFor(profileId)) {
      return std::nullopt;
    }
    return ProfileInlineEditRequest{*action_, profileId_, draft_};
  }

  void clearIfTargetUnavailable(bool targetAvailable) {
    if (active() && !targetAvailable) {
      clear();
    }
  }

  void clear() {
    action_.reset();
    profileId_.clear();
    draft_.clear();
  }

private:
  void begin(ProfileInlineEditAction action, std::string_view profileId,
             std::string_view draft) {
    action_ = action;
    profileId_ = profileId;
    draft_ = draft;
  }

  std::optional<ProfileInlineEditAction> action_;
  std::string profileId_;
  std::string draft_;
};

} // namespace settings_scene
~~~~

Add scene/SettingsSceneProfileEditorState.h beside the other Settings paths in the Xcode membershipExceptions list.

- [ ] **Step 4: Run the focused state test to verify GREEN**

Run:

~~~~bash
cmake --build cmake-build-debug --target view_layout_tests -j 6
ctest --test-dir cmake-build-debug -R '^view_layout_tests$' --output-on-failure
~~~~

Expected: view_layout_tests passes.

- [ ] **Step 5: Rewire Profile-tab controls to the tested state**

In SettingsScene.h, include the new header, rename the create-only fields, and add persistent state:

~~~~cpp
#include "SettingsSceneProfileEditorState.h"

TextInputBox *profileCreateNameInput = nullptr;
std::string profileCreateNameText;
settings_scene::ProfileInlineEditorState profileInlineEditor;
~~~~

In SettingsSceneProfiles.cpp:

1. Bind the top input and Create button only to profileCreateNameText.
2. Remove the top-card Cancel Confirmation button.
3. Render profileStatusText before the action cards as tab-level feedback and remove it from archiveBody:

~~~~cpp
const auto &status = profileController->status();
profileStatusText =
    makeWrappedText(status.message.empty() ? "Ready." : status.message,
                    metrics.bodyTextSize, ui_theme::textSecondary());
profileStatusText->setColor(statusColor(status.kind));
cardsColumn->addView(profileStatusText);
~~~~
4. Clear a stale editor target before rendering:

~~~~cpp
const bool editorTargetAvailable =
    !profileInlineEditor.active() ||
    std::ranges::any_of(profileController->profiles(), [&](const auto &item) {
      return profileInlineEditor.activeFor(item.id);
    });
profileInlineEditor.clearIfTargetUnavailable(editorTargetAvailable);
~~~~

5. Gate Select with idle. Make Rename/Duplicate begin an edit for their captured UUID and display name:

~~~~cpp
actions->addView(makeProfileActionButton(
    metrics, selected ? "Selected" : "Select", idle && !selected,
    [this, id = profile.id]() {
      profileController->select(id);
      invalidateProfileLayout();
    }));

actions->addView(makeProfileActionButton(
    metrics, "Rename", idle,
    [this, id = profile.id, name = profile.displayName]() {
      profileInlineEditor.beginRename(id, name);
      invalidateProfileLayout();
    }));
actions->addView(makeProfileActionButton(
    metrics, "Duplicate", idle,
    [this, id = profile.id, name = profile.displayName]() {
      profileInlineEditor.beginDuplicate(id, name);
      invalidateProfileLayout();
    }));
~~~~

6. When profileInlineEditor.activeFor(profile.id), append an input and Apply/Cancel row inside that card:

~~~~cpp
if (profileInlineEditor.activeFor(profile.id)) {
  auto *editorInput =
      makeTextInput(metrics, std::max(260, metrics.cardsWidth / 2));
  editorInput->setEditingText(std::string(profileInlineEditor.draft()));
  editorInput->onTextChanged(
      [this, id = profile.id](const std::string &text) {
        if (profileInlineEditor.activeFor(id)) {
          profileInlineEditor.updateDraft(text);
        }
      });
  body->addView(editorInput);

  auto *editorActions = new View();
  editorActions->setFlexDirection(FlexDirection::Row);
  editorActions->setFlexWrap(YGWrapWrap);
  editorActions->setGap(metrics.compact ? 8.0F : 10.0F);
  editorActions->addView(makeProfileActionButton(
      metrics, "Apply", true, [this, id = profile.id]() {
        const auto request = profileInlineEditor.requestFor(id);
        if (!request) {
          return;
        }
        const ProfileResult result =
            request->action ==
                    settings_scene::ProfileInlineEditAction::Rename
                ? profileController->rename(request->profileId, request->name)
                : profileController->duplicate(request->profileId,
                                               request->name);
        if (result.ok()) {
          profileInlineEditor.clear();
        }
        invalidateProfileLayout();
      }));
  editorActions->addView(makeProfileActionButton(
      metrics, "Cancel", true, [this]() {
        profileInlineEditor.clear();
        invalidateProfileLayout();
      }));
  body->addView(editorActions);
}
~~~~

7. For the target row in ConfirmDelete or ConfirmOverwrite, append the matching warning and a row-local Cancel button:

~~~~cpp
if (confirmingDelete || confirmingOverwrite) {
  body->addView(makeWrappedText(
      confirmingDelete
          ? "Delete this profile permanently?"
          : "Replace this profile from the selected archive?",
      metrics.bodyTextSize, ui_theme::amber()));
  actions->addView(makeProfileActionButton(
      metrics, "Cancel", true, [this]() {
        profileController->cancelConfirmation();
        invalidateProfileLayout();
      }));
}
~~~~

In SettingsSceneLayout.cpp, clear the inline editor when leaving the Profile tab:

~~~~cpp
if (activeTab == SettingsTab::Profile && profileController != nullptr) {
  const auto phase = profileController->phase();
  if (phase == ProfileSettingsPhase::PickingImport ||
      phase == ProfileSettingsPhase::Importing ||
      phase == ProfileSettingsPhase::PreparingExport ||
      phase == ProfileSettingsPhase::PickingExport) {
    return;
  }
  profileInlineEditor.clear();
  profileController->cancelConfirmation();
}
~~~~

In SettingsScene.cpp cleanupScene, call profileInlineEditor.clear(). Rename every profileNameInput reset to profileCreateNameInput and every profileNameText use to profileCreateNameText.

- [ ] **Step 6: Verify profile integration**

Run:

~~~~bash
cmake --build cmake-build-debug --target view_layout_tests profile_settings_controller_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(view_layout_tests|foundation_profile_controller)$' --output-on-failure
~~~~

Expected: both tests pass and main links.

- [ ] **Step 7: Commit the profile-card change**

~~~~bash
git add src/scene/SettingsSceneProfileEditorState.h \
  src/scene/SettingsScene.h src/scene/SettingsSceneProfiles.cpp \
  src/scene/SettingsSceneLayout.cpp src/scene/SettingsScene.cpp \
  tests/view_layout_tests.cpp \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "fix(settings): isolate profile card actions"
~~~~

---

### Task 2: Root-level binding-conflict modal

**Files:**
- Modify: src/view/BlockingOverlayView.h
- Modify: src/scene/SettingsScene.h
- Modify: src/scene/SettingsSceneInput.cpp
- Modify: src/scene/SettingsSceneLayout.cpp
- Modify: src/scene/SettingsScene.cpp
- Modify: tests/view_layout_tests.cpp

**Interfaces:**
- Consumes: InputCaptureController AwaitingConflictConfirmation, pendingConflicts, confirmReplace, and rejectReplace.
- Produces: SettingsScene::buildInputConflictOverlay(const LayoutMetrics&); inputConflictOverlayRoot is a root-level blocking sibling of scroll content.

- [ ] **Step 1: Write the failing overlay isolation test**

Include view/BlockingOverlayView.h in tests/view_layout_tests.cpp and add:

~~~~cpp
class EventRecordingView final : public View {
public:
  int eventCount = 0;

private:
  bool handleEventsImpl(SDL_Event &) override {
    ++eventCount;
    return true;
  }
};

void testBlockingOverlayStopsAllInteractiveEvents() {
  View root(0, 0, 640, 480);
  auto *background = new EventRecordingView();
  auto *overlay = new BlockingOverlayView(0, 0, 640, 480);
  root.addView(background);
  root.addView(overlay);

  constexpr std::array eventTypes{
      SDL_MOUSEBUTTONDOWN, SDL_MOUSEWHEEL, SDL_FINGERDOWN, SDL_KEYDOWN,
      SDL_TEXTINPUT, SDL_TEXTEDITING};
  for (const Uint32 eventType : eventTypes) {
    SDL_Event event{};
    event.type = eventType;
    assert(!root.handleEvents(event));
  }
  assert(background->eventCount == 0);

  overlay->setVisible(false);
  SDL_Event event{};
  event.type = SDL_TEXTINPUT;
  assert(root.handleEvents(event));
  assert(background->eventCount == 1);
}
~~~~

Call the test from main and add <array>.

- [ ] **Step 2: Run the focused test to verify RED**

~~~~bash
cmake --build cmake-build-debug --target view_layout_tests -j 6
ctest --test-dir cmake-build-debug -R '^view_layout_tests$' --output-on-failure
~~~~

Expected: assertion failure because text input reaches the background.

- [ ] **Step 3: Make the blocking primitive cover text entry**

Extend BlockingOverlayView.h:

~~~~cpp
case SDL_TEXTINPUT:
case SDL_TEXTEDITING:
  return false;
~~~~

Run Step 2 again. Expected: view_layout_tests passes.

- [ ] **Step 4: Move conflict composition out of the scroll column**

Delete the AwaitingConflictConfirmation card block from buildInputTab.

Add View *inputConflictOverlayRoot = nullptr and declare:

~~~~cpp
void buildInputConflictOverlay(
    const settings_scene::LayoutMetrics &metrics);
~~~~

Implement buildInputConflictOverlay in SettingsSceneInput.cpp:

~~~~cpp
void SettingsScene::buildInputConflictOverlay(const LayoutMetrics &metrics) {
  if (activeTab != SettingsTab::Input || inputCaptureController == nullptr ||
      inputCaptureController->state() !=
          InputCaptureController::State::AwaitingConflictConfirmation) {
    return;
  }

  inputConflictOverlayRoot = new BlockingOverlayView(
      0, 0, rendering::window_width, rendering::window_height);
  inputConflictOverlayRoot->setPositionType(YGPositionTypeAbsolute);
  inputConflictOverlayRoot->setPosition(Edge::Left, 0);
  inputConflictOverlayRoot->setPosition(Edge::Top, 0);
  inputConflictOverlayRoot->setZIndex(1050);
  inputConflictOverlayRoot->setFlexDirection(FlexDirection::Column);
  inputConflictOverlayRoot->setAlignItems(YGAlignCenter);
  inputConflictOverlayRoot->setJustifyContent(YGJustifyCenter);
  inputConflictOverlayRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(static_cast<float>(std::min(
      metrics.compact ? 620 : 760,
      std::max(280, metrics.contentWidth - 32))));
  panel->setMinHeight(static_cast<float>(metrics.compact ? 260 : 300));
  panel->setFlexDirection(FlexDirection::Column);
  panel->setAlignItems(YGAlignStretch);
  panel->setGap(metrics.compact ? 14.0F : 18.0F);
  panel->setPadding(Edge::All, static_cast<float>(metrics.cardPadding));
  panel->setThemedBackgroundColor(ui_theme::panelStrong);
  panel->setCornerRadius(ui_theme::panelRadius());
  panel->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow);
  panel->setThemedBorderColor(ui_theme::hairline);
  panel->setBorderWidth(1);
  panel->addView(makeWrappedText("Binding conflict",
                                 metrics.sectionTitleSize,
                                 ui_theme::textPrimary()));

for (const auto &conflict : inputCaptureController->pendingConflicts()) {
  panel->addView(makeWrappedText(
      controlLabel(conflict.control) + " is currently assigned to " +
          actionLabel(conflict.action) + " in this scope.",
      metrics.bodyTextSize, ui_theme::amber()));
}
  panel->addView(makeWrappedText(
      "The profile is unchanged until Replace is explicitly confirmed.",
      metrics.bodyTextSize, ui_theme::textSecondary()));

  auto *actions = new View();
  actions->setFlexDirection(FlexDirection::Row);
  actions->setFlexWrap(YGWrapWrap);
  actions->setGap(metrics.compact ? 10.0F : 14.0F);
  actions->setJustifyContent(YGJustifyCenter);
  auto *replaceButton = makeAccentButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Replace", metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE),
      ui_theme::amber());

replaceButton->setOnClickListener([this]() {
  inputCaptureController->confirmReplace();
  inputCaptureAction.reset();
  requestInputViewRebuild();
});
  actions->addView(replaceButton);

  auto *keepButton = makeControlButton(
      metrics.actionButtonWidth, metrics.actionButtonHeight,
      makeText("Keep existing", metrics.bodyTextSize + 2,
               ui_theme::textPrimary(), TextView::CENTER, TextView::MIDDLE));
keepButton->setOnClickListener([this]() {
  inputCaptureController->rejectReplace();
  inputCaptureAction.reset();
  requestInputViewRebuild();
});
  actions->addView(keepButton);
  panel->addView(actions);

  inputConflictOverlayRoot->addView(panel);
  rootLayout->addView(inputConflictOverlayRoot);
}
~~~~

Include BlockingOverlayView.h in SettingsSceneInput.cpp. In initView, call the builder after rootLayout receives content and before the display overlay:

~~~~cpp
rootLayout->addView(content);

buildDifficultyTableImportModal(metrics);
buildInputConflictOverlay(metrics);
buildDisplayPreviewOverlay(metrics);
~~~~

Set inputConflictOverlayRoot = nullptr in resetViewState and cleanupScene. Resize it in renderScene:

~~~~cpp
if (inputConflictOverlayRoot != nullptr) {
  inputConflictOverlayRoot->setSize(rendering::window_width,
                                    rendering::window_height);
}
~~~~

- [ ] **Step 5: Verify modal integration**

~~~~bash
cmake --build cmake-build-debug --target view_layout_tests input_capture_controller_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(view_layout_tests|foundation_input_capture)$' --output-on-failure
~~~~

Expected: both tests pass and main links.

- [ ] **Step 6: Commit the modal change**

~~~~bash
git add src/view/BlockingOverlayView.h src/scene/SettingsScene.h \
  src/scene/SettingsSceneInput.cpp src/scene/SettingsSceneLayout.cpp \
  src/scene/SettingsScene.cpp tests/view_layout_tests.cpp
git commit -m "fix(settings): show binding conflicts as modal"
~~~~

---

### Task 3: Integrated verification and review

**Files:**
- Review: all Task 1 and Task 2 changes.
- Modify only if verification exposes a defect.

**Interfaces:**
- Consumes: Task 1's UUID-bound editor and Task 2's root-level overlay.
- Produces: a clean feature branch with both UX fixes and no format/schema changes.

- [ ] **Step 1: Inspect the combined diff and iOS membership**

~~~~bash
git diff 3266707..HEAD --check
git diff 3266707..HEAD -- src/scene src/view tests \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
rg -n 'SettingsSceneProfileEditorState.h' \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
~~~~

Expected: no whitespace errors, the new header is in membershipExceptions, and no persistence files changed.

- [ ] **Step 2: Run focused Settings/input/profile verification**

~~~~bash
cmake --build cmake-build-debug --target \
  view_layout_tests profile_settings_controller_tests \
  input_capture_controller_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^(view_layout_tests|foundation_profile_controller|foundation_input_capture)$' \
  --output-on-failure
~~~~

Expected: three tests pass and main links.

- [ ] **Step 3: Run full desktop CTest**

~~~~bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
~~~~

Expected: zero failed tests; existing Yoga capability skips may remain skipped.

- [ ] **Step 4: Run iOS compile-only verification**

~~~~bash
scripts/ios_firebase_deploy.sh --build-only
~~~~

Expected: iOS app compiles successfully and nothing is uploaded.

- [ ] **Step 5: Review branch state**

~~~~bash
git status --short --branch
git log --oneline --decorate -5
~~~~

Expected: clean feature/player-foundations-local, ahead only by intentional local commits.
