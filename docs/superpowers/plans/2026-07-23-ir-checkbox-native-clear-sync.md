# IR Checkbox and Native Clear Synchronization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the boxed IR upload selector with a large tinted FontAwesome checkbox and keep the active UIKit text field synchronized when a common `TextInputBox` is cleared or updated programmatically.

**Architecture:** Keep the shared checkbox component and vary only its icon size and IR-specific button styling. Add a context-scoped native editor state setter to the existing iOS bridge, then make `TextInputBox` synchronize that state without dismissing the native editor.

**Tech Stack:** C++23, SDL2/SDL_ttf, Yoga, FontAwesome Free Solid, Objective-C++, UIKit, CMake/CTest, Python source audits.

## Global Constraints

- Preserve the IR selector's existing 34 by 34 hit target, selection callbacks, and locking behavior.
- Use a 30-pixel FontAwesome checkbox with cyan checked tint and muted unchecked tint.
- Render no IR selector background or border in normal, hover, or pressed states.
- Do not change either Club Beat control.
- Keep `TextInputBox` as the editing-text source of truth.
- Programmatic native updates must be scoped to the matching editor context and must not emit a duplicate text-change callback.
- Clearing must not dismiss the UIKit editor or keyboard.
- Do not run an iOS Firebase upload; verification uses build-only mode.

---

### Task 1: Large Tinted IR Upload Checkbox

**Files:**
- Modify: `src/view/Button.h`
- Modify: `src/view/TextView.h`
- Modify: `src/view/CheckboxButtonContent.cpp`
- Modify: `src/view/IrUploadCandidateListView.cpp`
- Test: `tests/ir_upload_candidate_list_view_tests.cpp`

**Interfaces:**
- Produces: `bool Button::hasStyledBackgroundStyle() const noexcept` for view-state verification.
- Produces: `int TextView::pointSize() const noexcept` and `SDL_Color TextView::currentColor() const noexcept` for rendered text-state verification.
- Preserves: `CheckboxButtonContent(std::string label, int labelSize, int iconSize)`; icon width becomes `max(24, iconSize + 4)`.

- [ ] **Step 1: Add failing IR visual-state assertions**

Include `UiTheme.h`, add an SDL color equality helper, and extend the selected
and rebound-unselected assertions:

```cpp
#include "../src/view/UiTheme.h"

bool sameColor(SDL_Color left, SDL_Color right) {
  return left.r == right.r && left.g == right.g && left.b == right.b &&
         left.a == right.a;
}

expect(selectionContent->iconView()->pointSize() == 30,
       "IR upload checkbox uses the large icon size");
expect(selectionContent->iconView()->getWidth() >= 30,
       "IR upload checkbox gives the large glyph enough layout width");
expect(!selection->hasStyledBackgroundStyle(),
       "IR upload checkbox has no outer selection box");
expect(sameColor(selectionContent->iconView()->currentColor(),
                 ui_theme::sdl(ui_theme::cyan())),
       "selected IR upload checkbox tints the glyph cyan");
```

After rebinding the row as unselected, assert:

```cpp
selectionContent = dynamic_cast<CheckboxButtonContent *>(
    selection->getContentView());
expect(!selection->hasStyledBackgroundStyle(),
       "unchecked IR upload checkbox remains unboxed");
expect(sameColor(selectionContent->iconView()->currentColor(),
                 ui_theme::sdl(ui_theme::textMuted())),
       "unchecked IR upload checkbox uses the muted tint");
```

- [ ] **Step 2: Run the test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target ir_upload_candidate_list_view_tests -j 6
```

Expected: compilation fails because `pointSize`, `currentColor`, and
`hasStyledBackground` do not exist.

- [ ] **Step 3: Add inspection accessors and make large glyphs fit**

Add the following inline accessors:

```cpp
// Button.h
[[nodiscard]] bool hasStyledBackgroundStyle() const noexcept {
  return hasStyledBackground;
}

// TextView.h
[[nodiscard]] int pointSize() const noexcept { return fontSize; }
[[nodiscard]] SDL_Color currentColor() const noexcept { return color; }
```

In `CheckboxButtonContent.cpp`, include `<algorithm>` and replace the fixed
icon width with:

```cpp
icon_->setWidth(std::max(24, iconSize + 4))->setHeight(54);
```

This preserves the 24-pixel width for the 16- and 17-pixel Club Beat icons and
allocates 34 pixels for the 30-pixel IR icon.

- [ ] **Step 4: Apply the IR-only icon styling**

Construct the IR content with a 30-pixel icon:

```cpp
selectionContent_ = new CheckboxButtonContent("", 20, 30);
```

Remove both calls to `selectionButton_->setThemedBackgroundColors` from
`setCandidate`. Keep the button's default transparent, borderless style and
set the glyph tint directly:

```cpp
if (selected) {
  selectionContent_->setThemedColor(ui_theme::cyan);
} else {
  selectionContent_->setThemedColor(ui_theme::textMuted);
}
```

- [ ] **Step 5: Run focused checkbox tests to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target \
  checkbox_button_content_tests ir_upload_candidate_list_view_tests \
  play_options_panel_view_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(checkbox_button_content_tests|ir_upload_candidate_list_view_tests|play_options_panel_view_tests)$' \
  --output-on-failure
```

Expected: all three tests pass, including unchanged Club Beat behavior.

- [ ] **Step 6: Commit the IR checkbox change**

```bash
git add src/view/Button.h src/view/TextView.h \
  src/view/CheckboxButtonContent.cpp src/view/IrUploadCandidateListView.cpp \
  tests/ir_upload_candidate_list_view_tests.cpp
git commit -m "fix: simplify IR upload checkbox"
```

---

### Task 2: Synchronize the Active UIKit Text Editor

**Files:**
- Create: `scripts/check_ios_native_text_editor_sync.py`
- Modify: `CMakeLists.txt`
- Modify: `src/iOSNatives.hpp`
- Modify: `src/iOSNatives.mm`
- Modify: `src/view/TextInputBox.h`
- Modify: `src/view/TextInputBox.cpp`
- Test: `tests/text_input_box_tests.cpp`

**Interfaces:**
- Produces: `void SetIOSNativeTextEditorState(void *context, const IOSNativeTextEditorState &state)`.
- Produces: `void TextInputBox::syncNativeTextEditorText()` on iOS and the iOS simulator.
- Preserves: one `onTextChanged` notification from `TextInputBox::clearFromButton()`.

- [ ] **Step 1: Add a failing native synchronization audit**

Create `scripts/check_ios_native_text_editor_sync.py` that loads
`src/iOSNatives.hpp`, `src/iOSNatives.mm`, and `src/view/TextInputBox.cpp`, then
requires all of the following:

```python
require(
    "void SetIOSNativeTextEditorState(" in header,
    "the iOS bridge must expose complete native editor state updates",
)
require(
    "- (void)setState:(const IOSNativeTextEditorState &)state;" in native,
    "the native editor view must accept programmatic text and selection",
)
require(
    "[gNativeTextEditor context] != editorContext" in native
    and "[gNativeTextEditor setState:editorState];" in native,
    "native editor updates must be scoped to the requesting input",
)
require(
    "syncNativeTextEditorText();" in text_input
    and text_input.count("syncNativeTextEditorText();") >= 2,
    "programmatic editing and clear-button paths must sync UIKit text",
)
```

Collect failures, print each with a `FAIL:` prefix, and exit nonzero when any
requirement is absent. Register it in `CMakeLists.txt` as
`ios_native_text_editor_sync_audit`, following the neighboring Python audit
tests.

- [ ] **Step 2: Run the audit to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target text_input_box_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^ios_native_text_editor_sync_audit$' --output-on-failure
```

Expected: the audit fails because the state setter and synchronization calls
do not exist.

- [ ] **Step 3: Implement the context-scoped UIKit state setter**

Declare in `iOSNatives.hpp`:

```cpp
void SetIOSNativeTextEditorState(void *context,
                                 const IOSNativeTextEditorState &state);
```

Add to the Objective-C interface and implementation:

```objective-c
- (void)setState:(const IOSNativeTextEditorState &)state;

- (void)setState:(const IOSNativeTextEditorState &)state {
  NSString *text = NSStringFromUtf8(state.text);
  _textField.text = text != nil ? text : @"";
  [self setSelectionStart:state.selectionStart end:state.selectionEnd];
}
```

Add the bridge function, copying the state before any asynchronous dispatch:

```objective-c
void SetIOSNativeTextEditorState(
    void *context, const IOSNativeTextEditorState &state) {
  void *editorContext = context;
  const IOSNativeTextEditorState editorState = state;
  auto stateBlock = ^{
    @autoreleasepool {
      if (gNativeTextEditor == nil) {
        return;
      }
      if (editorContext != nullptr &&
          [gNativeTextEditor context] != editorContext) {
        return;
      }
      [gNativeTextEditor setState:editorState];
    }
  };
  if ([NSThread isMainThread]) {
    stateBlock();
  } else {
    dispatch_async(dispatch_get_main_queue(), stateBlock);
  }
}
```

Assigning `UITextField.text` programmatically does not dispatch the registered
`UIControlEventEditingChanged` target, so this operation does not generate a
second C++ callback.

- [ ] **Step 4: Synchronize programmatic edits and clear-button edits**

Add this private iOS-only method to `TextInputBox`:

```cpp
void syncNativeTextEditorText();
```

Implement it as:

```cpp
void TextInputBox::syncNativeTextEditorText() {
  if (!nativeTextEditorVisible) {
    return;
  }
  SetIOSNativeTextEditorState(
      this, {.text = editingText,
             .selectionStart = selectionStart(),
             .selectionEnd = selectionEnd()});
}
```

Remove `hideNativeTextEditor(false)` from `setEditingText`. After updating the
C++ state and before `refreshDisplay(false)`, call
`syncNativeTextEditorText()` when building for iOS or the simulator.

In `clearFromButton`, after resetting the cursor and selection but before
`refreshDisplay(true)`, call `syncNativeTextEditorText()` under the same iOS
compile guard. This updates UIKit first and preserves exactly one normal C++
text-change callback.

- [ ] **Step 5: Strengthen the common clear regression**

Keep the existing callback-count assertion in `tests/text_input_box_tests.cpp`
and clarify its message:

```cpp
expect(notifications == 1 && lastText.empty(),
       "clear button publishes exactly one normal text change");
```

Run:

```bash
cmake --build cmake-build-debug --target text_input_box_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(text_input_box_tests|ios_native_text_editor_sync_audit)$' \
  --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 6: Commit the native synchronization change**

```bash
git add CMakeLists.txt scripts/check_ios_native_text_editor_sync.py \
  src/iOSNatives.hpp src/iOSNatives.mm \
  src/view/TextInputBox.h src/view/TextInputBox.cpp \
  tests/text_input_box_tests.cpp
git commit -m "fix: sync native editor when clearing input"
```

---

### Task 3: Final Verification and Publication

**Files:**
- Verify only; no additional source changes expected.

**Interfaces:**
- Consumes the completed IR checkbox and native editor synchronization changes.
- Produces a verified branch pushed to `origin/fix/ui-alignment`.

- [ ] **Step 1: Run the complete desktop suite**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: every configured test passes with zero failures.

- [ ] **Step 2: Compile the desktop application**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: the `main` target builds successfully.

- [ ] **Step 3: Compile the iOS integration without uploading**

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: the project Ruby is selected, iOS initialization completes, and the
app compiles without archive distribution or Firebase upload.

- [ ] **Step 4: Check repository hygiene and push**

```bash
git diff --check
git status --short
git push origin fix/ui-alignment
test "$(git rev-parse HEAD)" = "$(git rev-parse origin/fix/ui-alignment)"
```

Expected: no uncommitted files remain and the remote branch matches local
`HEAD`.
