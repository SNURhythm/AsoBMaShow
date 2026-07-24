# Difficulty-Table URL Completion Identity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent a completed difficulty-table import from clearing a different URL typed after that import started.

**Architecture:** Capture the submitted URL in `SettingsScene` when an add starts. Extend the existing pure completion helper to clear only when a finished successful import's submitted URL still matches the current field value, then consume the captured identity after completion.

**Tech Stack:** C++23, CMake/CTest, `TextInputBox`, `SettingsScene`.

## Global Constraints

- Continue in-place on `fix/ui-alignment`.
- Keep the URL field editable while an import runs.
- Clear only a finished, successful import whose submitted URL equals the current URL text.
- Preserve newer text and all failed, cancelled, or unfinished values.
- Consume submitted identity after a finished completion.
- Do not reply to or resolve GitHub review threads unless separately requested.

---

### Task 1: Make URL clearing conditional on submitted identity

**Files:**
- Modify: `tests/difficulty_table_url_completion_tests.cpp`
- Modify: `src/scene/DifficultyTableUrlCompletion.h`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsSceneTables.cpp`

**Interfaces:**
- Consumes: the URL captured by `addDifficultyTableFromUrl()` and the current `tableUrlText`.
- Produces: `bool settings_ui::applyDifficultyTableUrlCompletion(bool finished, bool succeeded, std::string_view submittedUrl, std::string &currentUrl)`.

- [ ] **Step 1: Write the failing submitted-identity regression test**

Replace the existing helper calls with a submitted URL argument and add the changed-field case:

```cpp
const std::string submitted = "https://example.com/table.html";
std::string url = submitted;

expect(!settings_ui::applyDifficultyTableUrlCompletion(
           false, false, submitted, url),
       "an in-progress import does not clear the URL");
expect(url == submitted, "an in-progress import preserves the URL text");

expect(!settings_ui::applyDifficultyTableUrlCompletion(
           true, false, submitted, url),
       "a failed import does not clear the URL");
expect(url == submitted, "a failed import preserves the URL text");

url = "https://example.com/next-table.html";
expect(!settings_ui::applyDifficultyTableUrlCompletion(
           true, true, submitted, url),
       "a successful import does not clear a newer URL");
expect(url == "https://example.com/next-table.html",
       "a newer URL survives completion of the previous import");

url = submitted;
expect(settings_ui::applyDifficultyTableUrlCompletion(
           true, true, submitted, url),
       "a successful matching import reports that the URL was cleared");
expect(url.empty(), "a successful matching import clears the URL text");
```

- [ ] **Step 2: Build the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target difficulty_table_url_completion_tests -j 6
```

Expected: compilation fails because `applyDifficultyTableUrlCompletion` does not yet accept the submitted URL.

- [ ] **Step 3: Implement the minimal identity-aware helper**

Update `src/scene/DifficultyTableUrlCompletion.h`:

```cpp
#include <string_view>

inline bool applyDifficultyTableUrlCompletion(bool finished, bool succeeded,
                                              std::string_view submittedUrl,
                                              std::string &currentUrl) {
  if (!finished || !succeeded || submittedUrl.empty() ||
      currentUrl != submittedUrl) {
    return false;
  }

  currentUrl.clear();
  return true;
}
```

- [ ] **Step 4: Capture and consume the submitted URL in the scene**

Add this member next to `tableUrlText` in `src/scene/SettingsScene.h`:

```cpp
std::string submittedDifficultyTableUrl;
```

In `addDifficultyTableFromUrl()`, assign it immediately after input validation and before starting the background job:

```cpp
submittedDifficultyTableUrl = url;
```

In `applyPendingDifficultyTableUpdates()`, pass it to the helper and consume it after a finished completion:

```cpp
const bool clearedUrl = settings_ui::applyDifficultyTableUrlCompletion(
    completedImportFinished, completedImportSucceeded,
    submittedDifficultyTableUrl, tableUrlText);
if (completedImportFinished) {
  submittedDifficultyTableUrl.clear();
}
if (clearedUrl && tableUrlInput != nullptr) {
  tableUrlInput->setEditingText(tableUrlText);
}
```

- [ ] **Step 5: Verify the focused behavior and build**

Run:

```bash
cmake --build cmake-build-debug --target difficulty_table_url_completion_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^difficulty_table_url_completion_tests$'
```

Expected: the test passes `1/1`, and `main` builds successfully.

- [ ] **Step 6: Run the complete local suite and inspect scope**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
git diff -- tests/difficulty_table_url_completion_tests.cpp \
  src/scene/DifficultyTableUrlCompletion.h \
  src/scene/SettingsScene.h src/scene/SettingsSceneTables.cpp
```

Expected: all 141 tests pass, whitespace validation is clean, and the implementation diff contains only submitted-identity behavior and its regression test.

- [ ] **Step 7: Commit and push the review fix**

Run:

```bash
git add tests/difficulty_table_url_completion_tests.cpp \
  src/scene/DifficultyTableUrlCompletion.h \
  src/scene/SettingsScene.h src/scene/SettingsSceneTables.cpp
git commit -m "fix: preserve newly typed table URL"
git push origin fix/ui-alignment
```

Expected: the existing PR branch advances with the focused review fix and no unrelated files.

- [ ] **Step 8: Verify the pushed PR state**

Run:

```bash
gh pr checks 79 --watch --fail-fast
git fetch origin fix/ui-alignment
git status --short --branch
```

Expected: required PR checks pass and the clean local branch matches `origin/fix/ui-alignment`.
