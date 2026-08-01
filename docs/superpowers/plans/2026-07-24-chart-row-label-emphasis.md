# Chart Row Label Emphasis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render chart-row difficulty and key-mode labels in bold while preserving all existing content, layout, color, banner, and theme behavior.

**Architecture:** Add an optional semantic font weight to `TextView`, translate it to an SDL_ttf style, and include that style in the shared font-cache key so bold faces cannot mutate regular faces. Configure only the chart row's level and key-mode views as bold and expose their identities to the focused row test.

**Tech Stack:** C++23, SDL_ttf, bgfx headless tests, CTest, git/GitHub.

## Global Constraints

- Existing `TextView` constructor call sites remain regular by default.
- Difficulty and key-mode labels use `TextView::FontWeight::Bold` in every chart-row state.
- Font cache acquisition and release use the same path, raster size, and SDL_ttf style key.
- Do not add a font asset or change label size, color, alignment, text, or layout.
- Preserve all existing banner fade and readability scrim behavior.

---

### Task 1: Add semantic text weight and configure chart labels

**Files:**
- Modify: `tests/chart_list_item_view_tests.cpp`
- Modify: `src/view/TextView.h`
- Modify: `src/view/TextView.cpp`
- Modify: `src/view/ChartListItemView.cpp`

**Interfaces:**
- Produces: `TextView::FontWeight::{Regular,Bold}`, `TextView(const std::string &, int, FontWeight = FontWeight::Regular)`, and `FontWeight fontWeight() const noexcept`.
- Consumes: SDL_ttf `TTF_STYLE_NORMAL`, `TTF_STYLE_BOLD`, and `TTF_SetFontStyle()`.

- [ ] **Step 1: Write the failing chart-label weight test**

Find the named labels after constructing the row and assert their semantic weight:

```cpp
auto *difficulty = dynamic_cast<TextView *>(
    row.findViewByName("chartListDifficulty"));
auto *keyMode = dynamic_cast<TextView *>(
    row.findViewByName("chartListKeyMode"));
require(difficulty != nullptr && keyMode != nullptr,
        "chart row exposes difficulty and key-mode labels");
require(difficulty->fontWeight() == TextView::FontWeight::Bold &&
            keyMode->fontWeight() == TextView::FontWeight::Bold,
        "difficulty and key-mode labels use bold text");
```

- [ ] **Step 2: Run the target to verify the test fails**

```bash
cmake --build cmake-build-debug --target chart_list_item_view_tests -j 6
```

Expected: compilation fails because `FontWeight` and `fontWeight()` do not exist.

- [ ] **Step 3: Add style-separated font caching to `TextView`**

Add the public semantic API and private state in `TextView.h`:

```cpp
enum class FontWeight { Regular, Bold };
TextView(const std::string &fontPath, int fontSize,
         FontWeight fontWeight = FontWeight::Regular);
[[nodiscard]] FontWeight fontWeight() const noexcept { return fontWeight_; }

FontWeight fontWeight_ = FontWeight::Regular;
int fontStyle_ = TTF_STYLE_NORMAL;
```

In `TextView.cpp`, map the enum and make the cache key style-aware:

```cpp
int fontStyleForWeight(TextView::FontWeight weight) {
  return weight == TextView::FontWeight::Bold ? TTF_STYLE_BOLD
                                               : TTF_STYLE_NORMAL;
}

std::string fontCacheKey(const std::string &path, int fontSize,
                         int fontStyle) {
  return path + "#" + std::to_string(fontSize) + "#" +
         std::to_string(fontStyle);
}
```

Pass `fontStyle` through `acquireFontCandidate()` and `releaseFontCandidate()`. Immediately after a successful `TTF_OpenFont`, apply:

```cpp
TTF_SetFontStyle(opened, fontStyle);
```

Initialize `fontWeight_` and `fontStyle_` before loading the first face, pass `fontStyle_` through `loadFallbackFontAt()`, and release every `FontFace` with the same `fontStyle_`.

- [ ] **Step 4: Configure and name the two chart labels**

Construct and name the views in `ChartListItemView.cpp`:

```cpp
levelView = new TextView(kUiFont, 18, TextView::FontWeight::Bold);
keyModeView = new TextView(kUiFont, 14, TextView::FontWeight::Bold);
levelView->setName("chartListDifficulty");
keyModeView->setName("chartListKeyMode");
```

- [ ] **Step 5: Build and run the focused test**

```bash
cmake --build cmake-build-debug --target chart_list_item_view_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R 'image_fade_shader_audit|image_fade_tests|image_view_fade_tests|chart_list_item_view_tests'
```

Expected: 4/4 tests pass.

- [ ] **Step 6: Commit the implementation**

```bash
git add src/view/TextView.h src/view/TextView.cpp \
  src/view/ChartListItemView.cpp tests/chart_list_item_view_tests.cpp
git commit -m "feat: bold chart difficulty labels"
```

### Task 2: Verify and publish the branch

**Files:**
- Verify and publish only; no expected source changes.

**Interfaces:**
- Consumes: the complete `fix/ui-alignment` branch and authenticated GitHub remote.
- Produces: fresh verification evidence and an updated remote PR branch.

- [ ] **Step 1: Run complete verification**

```bash
python3 scripts/check_image_fade_shader.py
cmake --build cmake-build-debug -j 6
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
git status --short
```

Expected: shader audit and both builds exit 0, 140/140 tests pass, and the worktree is clean.

- [ ] **Step 2: Confirm GitHub scope and authentication**

```bash
gh --version
gh auth status
git status -sb
git log --oneline origin/fix/ui-alignment..HEAD
```

Expected: `gh` is authenticated, the worktree is clean, and only the intended banner-readability and label-emphasis commits are ahead.

- [ ] **Step 3: Push the existing branch**

```bash
git push -u origin fix/ui-alignment
```

Expected: push succeeds and updates the existing pull request branch without creating a second PR.

