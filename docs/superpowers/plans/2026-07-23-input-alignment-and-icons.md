# Input Alignment and FontAwesome Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pad input text consistently, add reusable clearable input behavior, replace Unicode checkbox marks with FontAwesome icons, and clear a successfully imported difficulty-table URL.

**Architecture:** Make `TextView` honor its Yoga content insets so every part of `TextInputBox` shares one padded coordinate system. Keep the clear control owned and rendered by `TextInputBox` because Yoga measured nodes cannot own layout children; expose the behavior through `setClearable(bool)`. Add a small reusable checkbox-content view for mixed FontAwesome icon and normal-font labels, then use a pure completion helper to make success-only difficulty-table URL clearing directly testable.

**Tech Stack:** C++23, SDL2/SDL_ttf, bgfx, Yoga layout, FontAwesome Free Solid, CMake/CTest.

## Global Constraints

- Apply 12 pixels of default horizontal padding to `TextInputBox`.
- Show the embedded FontAwesome x-mark only when a clearable field is non-empty.
- Clearing through the embedded button must invoke normal text-change callbacks and consume pointer/touch input.
- Enable clearing only for main chart search, Music Player library/favorites search, and difficulty-table URL.
- A successful difficulty-table add clears stored and visible URL state; failure or cancellation preserves it.
- Use FontAwesome square and square-check glyphs for IR upload and both Club Beat controls; keep labels in the normal UI font.
- Do not edit amalgamated BMS parser sources.

---

### Task 1: Padded and Clearable `TextInputBox`

**Files:**
- Create: `tests/text_input_box_tests.cpp`
- Modify: `src/view/TextView.h`
- Modify: `src/view/TextView.cpp`
- Modify: `src/view/TextInputBox.h`
- Modify: `src/view/TextInputBox.cpp`
- Modify: `src/view/IconText.h`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `View::getContentX()`, `getContentY()`, `getContentWidth()`, and `getContentHeight()`.
- Produces: `ui_icons::kXmark` for the common clear icon.
- Produces: `void TextInputBox::setClearable(bool clearable)` and `bool TextInputBox::isClearButtonVisible() const noexcept`.
- Produces: padded `TextView::resolvedTextRect()` coordinates used by text, caret, selection, hit-testing, composition, and IME placement.

- [ ] **Step 1: Add failing padding and clear-button behavior tests**

Create a headless bgfx test with an inspecting subclass and mouse helper:

```cpp
class InspectableTextInputBox final : public TextInputBox {
public:
  using TextInputBox::TextInputBox;
  SDL_Rect textRect() const { return resolvedTextRect(); }
};

void click(TextInputBox &input, int x, int y) {
  SDL_Event down{};
  down.type = SDL_MOUSEBUTTONDOWN;
  down.button.type = SDL_MOUSEBUTTONDOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.which = 1;
  down.button.x = x;
  down.button.y = y;
  SDL_Event up = down;
  up.type = SDL_MOUSEBUTTONUP;
  input.handleEvents(down);
  input.handleEvents(up);
}
```

Cover these assertions:

```cpp
InspectableTextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
input.setSize(240, 52);
input.applyYogaLayout();
input.setEditingText("query");
expect(input.textRect().x == input.getX() + 12,
       "left-aligned text uses the default leading inset");

int notifications = 0;
std::string lastText;
input.onTextChanged([&](const std::string &text) {
  ++notifications;
  lastText = text;
});
input.setClearable(true);
expect(input.isClearButtonVisible(),
       "configured non-empty input shows its clear button");
click(input, input.getX() + input.getWidth() - 18,
      input.getY() + input.getHeight() / 2);
expect(input.getText().empty() && notifications == 1 && lastText.empty(),
       "clear button clears and publishes one normal change");
expect(!input.isClearButtonVisible(),
       "clear button hides after the field becomes empty");

input.onUnselected();
click(input, input.getX() + input.getWidth() - 18,
      input.getY() + input.getHeight() / 2);
expect(input.getSelected(),
       "empty input has no clear-button hit target at the trailing edge");
```

- [ ] **Step 2: Register and run the new test to verify RED**

Add `text_input_box_tests` with `Button.cpp`, `TextInputBox.cpp`, `TextView.cpp`, `View.cpp`, rendering support, `${COMMON_LIBS}`, `bgfx`, and `yogacore`; register it with `asobmashow_register_test`.

Run:

```bash
cmake --build cmake-build-debug --target text_input_box_tests -j 6
```

Expected: compilation fails because `setClearable`, `isClearButtonVisible`, and padded text geometry do not exist yet.

- [ ] **Step 3: Make `TextView` resolve and clip inside content insets**

Update `resolvedTextRect()` to start from the content box:

```cpp
const int contentHeight = rect.h > 0 ? rect.h : textLineHeight();
SDL_Rect drawRect = {getContentX(), getContentY(), rect.w, contentHeight};
const int width = getContentWidth();
const int height = getContentHeight();
```

Use `getContentX()`, `getContentY()`, `getContentWidth()`, and
`getContentHeight()` for hidden/marquee scissoring and available-width
comparisons so padding affects drawing without stretching the glyph texture.
Keep `resolvedTextRect()` protected for the inspecting test.

- [ ] **Step 4: Implement the common clearable property and internal control**

Add `inline constexpr uint32_t kXmark = 0xf00d;` to `IconText.h`, then add to
`TextInputBox`:

```cpp
void setClearable(bool clearable);
[[nodiscard]] bool isClearButtonVisible() const noexcept;

std::unique_ptr<Button> clearButton;
bool clearable = false;
void syncClearButton();
void syncClearButtonFrame();
void clearFromButton();
```

Forward-declare `Button` in the header. In the constructor, set left and right
padding to 12. Lazily create a 40-pixel-wide transparent `Button` whose content
is a centered `TextView(ui_icons::kFontAwesomeSolidPath, ...)` containing
`ui_icons::textForCodepoint(ui_icons::kXmark)`. Keep it outside Yoga's child
list, position it against the trailing edge from `onMove`/`onResize`, render it
after the input text, and offer it events before the normal input handler.

Use this state transition:

```cpp
void TextInputBox::clearFromButton() {
  if (editingText.empty() && composition.empty()) {
    return;
  }
  editingText.clear();
  clearComposition();
  cursorPos = 0;
  selectionAnchor = 0;
  refreshDisplay(true);
}

void TextInputBox::syncClearButton() {
  const bool visible = clearable && !displayedText().empty();
  clearButton->setVisible(visible);
  View::setPadding(Edge::Right, visible ? 48.0f : 12.0f);
}
```

Call `syncClearButton()` from `setClearable`, `setEditingText`, and
`refreshDisplay`. Propagate theme changes to the internally owned button.

- [ ] **Step 5: Run the focused test to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target text_input_box_tests -j 6
ctest --test-dir cmake-build-debug -R '^text_input_box_tests$' --output-on-failure
```

Expected: one test runs and passes.

- [ ] **Step 6: Commit the input component change**

```bash
git add CMakeLists.txt tests/text_input_box_tests.cpp \
  src/view/IconText.h \
  src/view/TextView.h src/view/TextView.cpp \
  src/view/TextInputBox.h src/view/TextInputBox.cpp
git commit -m "feat: add padded clearable text inputs"
```

---

### Task 2: Reusable FontAwesome Checkbox Content

**Files:**
- Create: `src/view/CheckboxButtonContent.h`
- Create: `src/view/CheckboxButtonContent.cpp`
- Create: `tests/checkbox_button_content_tests.cpp`
- Modify: `src/view/IconText.h`
- Modify: `src/view/CMakeLists.txt`
- Modify: `src/view/IrUploadCandidateListView.h`
- Modify: `src/view/IrUploadCandidateListView.cpp`
- Modify: `src/view/PlayOptionsPanelView.h`
- Modify: `src/view/PlayOptionsPanelView.cpp`
- Modify: `src/scene/MusicPlayerScene.h`
- Modify: `src/scene/MusicPlayerScene.cpp`
- Modify: `tests/ir_upload_candidate_list_view_tests.cpp`
- Modify: `tests/play_options_panel_view_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ui_icons::kFontAwesomeSolidPath` and `ui_icons::textForCodepoint`.
- Produces: `ui_icons::kSquare` and `ui_icons::kSquareCheck` constants.
- Produces: `CheckboxButtonContent(std::string label, int labelSize, int iconSize)`, `void setChecked(bool)`, `bool checked() const noexcept`, `void setThemedColor(ThemeColorProvider)`, `TextView *iconView()`, and `TextView *labelView()`.

- [ ] **Step 1: Write the failing reusable checkbox test**

Create a headless view test that asserts separate fonts/content and state:

```cpp
CheckboxButtonContent content("Club Beat", 18, 17);
content.setSize(220, 54);
content.applyYogaLayout();
expect(content.labelView()->getText() == "Club Beat",
       "checkbox keeps its label in a separate text view");
expect(content.iconView()->getText() ==
           ui_icons::textForCodepoint(ui_icons::kSquare),
       "unchecked state uses FontAwesome square");
content.setChecked(true);
expect(content.iconView()->getText() ==
           ui_icons::textForCodepoint(ui_icons::kSquareCheck),
       "checked state uses FontAwesome square-check");
```

Also extend existing view tests:

```cpp
auto *selectionContent = dynamic_cast<CheckboxButtonContent *>(
    selection->getContentView());
expect(selectionContent != nullptr && selectionContent->checked(),
       "IR upload selection uses common FontAwesome checkbox content");

auto *clubButton =
    dynamic_cast<Button *>(panel->findViewByName("club-mode"));
auto *clubContent = dynamic_cast<CheckboxButtonContent *>(
    clubButton == nullptr ? nullptr : clubButton->getContentView());
require(clubContent != nullptr && !clubContent->checked(),
        "Club Beat starts with the FontAwesome unchecked icon");
panel->refresh({.clubMode = true});
require(clubContent->checked(),
        "Club Beat refresh switches to the FontAwesome checked icon");
```

- [ ] **Step 2: Register/build tests to verify RED**

Add `checkbox_button_content_tests`, add `CheckboxButtonContent.cpp` to the IR
and Play Options test targets, and register the new target. Run:

```bash
cmake --build cmake-build-debug --target \
  checkbox_button_content_tests ir_upload_candidate_list_view_tests \
  play_options_panel_view_tests -j 6
```

Expected: compilation fails because the common checkbox content and icon
constants are not defined.

- [ ] **Step 3: Implement `CheckboxButtonContent`**

Build the content as a centered horizontal `View` containing:

```cpp
icon = new TextView(ui_icons::kFontAwesomeSolidPath, iconSize);
icon->setWidth(24)->setHeight(54);
icon->setAlign(TextView::CENTER);
icon->setVAlign(TextView::MIDDLE);

label = new TextView("assets/fonts/notosanscjkjp.ttf", labelSize);
label->setText(std::move(text));
label->setVAlign(TextView::MIDDLE);
```

Set a 7-pixel gap, hide the label when it is empty, and have `setChecked`
select `kSquareCheck` or `kSquare`. Apply themed color providers to both icon
and label.

- [ ] **Step 4: Migrate the requested checkbox call sites**

Use an empty-label `CheckboxButtonContent` in
`IrUploadCandidateListItemView`; call `setChecked(selected)` during rebinding.

For Play Options, name the button `club-mode`, retain the content pointer, and
replace Unicode text updates with:

```cpp
clubModeButtonContent->setChecked(state.clubMode);
styleCheckboxButton(clubModeButton, clubModeButtonContent,
                    state.clubMode, true);
```

For Music Player, construct the Club Beat button with common checkbox content,
store `CheckboxButtonContent *clubModeButtonContent`, and have
`refreshClubModeControl()` call `setChecked(enabled)` while applying the same
theme colors to both icon and label. Remove every `☐` and `☑` Club Beat string.

- [ ] **Step 5: Run the checkbox tests to verify GREEN**

```bash
cmake --build cmake-build-debug --target \
  checkbox_button_content_tests ir_upload_candidate_list_view_tests \
  play_options_panel_view_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(checkbox_button_content_tests|ir_upload_candidate_list_view_tests|play_options_panel_view_tests)$' \
  --output-on-failure
```

Expected: three tests run and pass.

- [ ] **Step 6: Commit the checkbox migration**

```bash
git add CMakeLists.txt src/view/CMakeLists.txt \
  src/view/IconText.h src/view/CheckboxButtonContent.h \
  src/view/CheckboxButtonContent.cpp \
  src/view/IrUploadCandidateListView.h \
  src/view/IrUploadCandidateListView.cpp \
  src/view/PlayOptionsPanelView.h src/view/PlayOptionsPanelView.cpp \
  src/scene/MusicPlayerScene.h src/scene/MusicPlayerScene.cpp \
  tests/checkbox_button_content_tests.cpp \
  tests/ir_upload_candidate_list_view_tests.cpp \
  tests/play_options_panel_view_tests.cpp
git commit -m "fix: use FontAwesome checkbox icons"
```

---

### Task 3: Clearable Call Sites and Successful Table URL Reset

**Files:**
- Create: `src/scene/DifficultyTableUrlCompletion.h`
- Create: `tests/difficulty_table_url_completion_tests.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/MusicPlayerScene.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneTables.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `TextInputBox::setClearable(bool)` from Task 1.
- Produces: `bool settings_ui::applyDifficultyTableUrlCompletion(bool finished, bool succeeded, std::string &url)`; returns true only when it cleared the stored URL.

- [ ] **Step 1: Write the failing difficulty-table completion policy test**

```cpp
std::string url = "https://example.test/table.html";
expect(!settings_ui::applyDifficultyTableUrlCompletion(false, false, url) &&
           !url.empty(),
       "in-progress import preserves URL");
expect(!settings_ui::applyDifficultyTableUrlCompletion(true, false, url) &&
           !url.empty(),
       "failed import preserves URL");
expect(settings_ui::applyDifficultyTableUrlCompletion(true, true, url) &&
           url.empty(),
       "successful import clears URL");
```

- [ ] **Step 2: Register and run the policy test to verify RED**

Add and register `difficulty_table_url_completion_tests`, then run:

```bash
cmake --build cmake-build-debug --target \
  difficulty_table_url_completion_tests -j 6
```

Expected: compilation fails because `DifficultyTableUrlCompletion.h` does not
exist.

- [ ] **Step 3: Implement success-only URL completion**

Create the header-only helper:

```cpp
namespace settings_ui {
inline bool applyDifficultyTableUrlCompletion(bool finished, bool succeeded,
                                              std::string &url) {
  if (!finished || !succeeded) {
    return false;
  }
  url.clear();
  return true;
}
} // namespace settings_ui
```

In `applyPendingDifficultyTableUpdates`, capture the newly applied finished and
succeeded flags while holding the mutex. After releasing it, call the helper;
when it returns true and `tableUrlInput` is non-null, call
`tableUrlInput->setEditingText(tableUrlText)`. This keeps UI mutation on the UI
thread and preserves the field on failure/cancellation.

- [ ] **Step 4: Enable clearability at the four approved call sites**

Add `setClearable(true)` for:

```cpp
searchBox                         // MainMenuScene
searchInput                       // both Music Player browser instances
tableUrlInput                     // Settings difficulty-table URL
```

Delete the standalone Music Player `Clear` button and its callback from
`buildTrackBrowserPage`; the embedded clear callback already updates the
search/filter through `onTextChanged`.

- [ ] **Step 5: Run focused tests and source checks to verify GREEN**

```bash
cmake --build cmake-build-debug --target \
  difficulty_table_url_completion_tests text_input_box_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^(difficulty_table_url_completion_tests|text_input_box_tests)$' \
  --output-on-failure
rg -n 'setClearable\(true\)' \
  src/scene/MainMenuScene.cpp src/scene/MusicPlayerScene.cpp \
  src/scene/SettingsSceneLayout.cpp
rg -n '☐|☑|makeButton\("Clear", 17, &clearSearchText\)' \
  src/view/IrUploadCandidateListView.cpp \
  src/view/PlayOptionsPanelView.cpp src/scene/MusicPlayerScene.cpp
```

Expected: two tests pass; the first search reports exactly three source lines
(one Music Player construction covers both instances); the obsolete-character
search returns no matches.

- [ ] **Step 6: Commit the call-site and URL behavior**

```bash
git add CMakeLists.txt src/scene/DifficultyTableUrlCompletion.h \
  tests/difficulty_table_url_completion_tests.cpp \
  src/scene/MainMenuScene.cpp src/scene/MusicPlayerScene.cpp \
  src/scene/SettingsSceneLayout.cpp src/scene/SettingsSceneTables.cpp
git commit -m "fix: clear searchable input fields"
```

---

### Task 4: Full Verification and Publish

**Files:**
- Verify only; do not add unrelated changes.

**Interfaces:**
- Consumes: all prior task outputs.
- Produces: verified commits pushed on `fix/ui-alignment`.

- [ ] **Step 1: Run all focused UI tests fresh**

```bash
cmake --build cmake-build-debug --target \
  text_input_box_tests checkbox_button_content_tests \
  difficulty_table_url_completion_tests ir_upload_candidate_list_view_tests \
  play_options_panel_view_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(text_input_box_tests|checkbox_button_content_tests|difficulty_table_url_completion_tests|ir_upload_candidate_list_view_tests|play_options_panel_view_tests)$' \
  --output-on-failure
```

Expected: five tests run, zero failures.

- [ ] **Step 2: Run the repository-required desktop build**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: target `main` builds successfully with exit status 0.

- [ ] **Step 3: Audit the final diff and scope**

```bash
git diff --check origin/fix/ui-alignment...HEAD
git status --short --branch
git log --oneline origin/fix/ui-alignment..HEAD
```

Expected: no whitespace errors, no unstaged implementation files, and only
the design, plan, and requested UI commits are ahead of the remote.

- [ ] **Step 4: Push the existing branch**

```bash
git push origin fix/ui-alignment
```

Expected: remote `fix/ui-alignment` advances to the final local commit.
