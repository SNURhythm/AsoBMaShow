# Compact Mobile Result Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep result controls visible on short mobile viewports by placing gauge history and timing analytics side-by-side, compact result cards only on short mobile, and use a two-by-two graph/analytics grid in photo exports.

**Architecture:** Add a header-only geometry policy that resolves regular versus compact result metrics from viewport height and target kind. `DefaultSkin` consumes it for live result composition, `ResultScene` tells the skin whether analytics exists, and `ResultImageExporter` builds the photo-only two-by-two grid from the same policy. Replay-video exports retain their current layout.

**Tech Stack:** C++23, Yoga, SDL2, bgfx, CMake/CTest.

## Global Constraints

- Compact cards apply only to mobile targets at logical viewport heights of 920 units or less.
- Desktop card dimensions remain unchanged.
- Live result visuals use 40 percent width for gauge history and 60 percent for timing analytics.
- Action buttons remain 64 logical units high.
- Photo exports retain Histogram, Lanes, and Sections.
- Replay-video result screens are unchanged.

---

### Task 1: Pure Result Layout Geometry

**Files:**
- Create: `src/scene/ResultLayoutGeometry.h`
- Create: `tests/result_layout_geometry_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: viewport height, mobile-target flag, drawable dimensions.
- Produces: `result_layout::Metrics`, `metricsFor`, `screenContentHeight`, `photoContentHeight`, `photoCanvasPixelHeight`, and `photoVisualOrder`.

- [ ] **Step 1: Register and write the failing geometry test**

Create a CTest executable beside `practice_analytics_presentation_tests`. The test must assert these exact outcomes:

```cpp
#include "scene/ResultLayoutGeometry.h"
#include <cassert>

int main() {
  const auto mobile = result_layout::metricsFor(885.0f, true);
  assert(mobile.compact);
  assert(mobile.rootPadding == 24.0f && mobile.rootGap == 8.0f);
  assert(mobile.summaryHeight == 184.0f);
  assert(mobile.infoHeight == 90.0f);
  assert(mobile.detailsHeight == 96.0f);
  assert(mobile.visualHeight == 236.0f);
  assert(result_layout::screenContentHeight(mobile, true) <= 885.0f);

  const auto desktop = result_layout::metricsFor(1080.0f, false);
  assert(!desktop.compact);
  assert(desktop.summaryHeight == 198.0f);
  assert(desktop.infoHeight == 100.0f);
  assert(desktop.detailsHeight == 108.0f);

  using result_layout::PhotoVisual;
  constexpr auto order = result_layout::photoVisualOrder();
  assert(order[0] == PhotoVisual::Gauge);
  assert(order[1] == PhotoVisual::Histogram);
  assert(order[2] == PhotoVisual::Lanes);
  assert(order[3] == PhotoVisual::Sections);
  assert(result_layout::photoContentHeight(mobile) <= 885.0f);
  assert(result_layout::photoCanvasPixelHeight(2532, 1170, 1920.0f,
                                               mobile) == 1170);
}
```

- [ ] **Step 2: Run the test and verify RED**

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target result_layout_geometry_tests -j 6
```

Expected: compilation fails because `scene/ResultLayoutGeometry.h` is missing.

- [ ] **Step 3: Implement the minimal geometry policy**

Create a header-only policy with this public shape and values:

```cpp
namespace result_layout {
inline constexpr float kShortMobileMaximumHeight = 920.0f;
inline constexpr float kHeaderHeight = 96.0f;
inline constexpr float kActionHeight = 64.0f;
inline constexpr float kLegacyGraphHeight = 136.0f;
enum class PhotoVisual { Gauge, Histogram, Lanes, Sections };

struct Metrics {
  bool compact = false;
  float rootPadding = 32.0f;
  float rootGap = 12.0f;
  float summaryHeight = 198.0f;
  float summaryPanelPadding = 14.0f;
  float gradePanelPadding = 12.0f;
  float infoHeight = 100.0f;
  float infoTilePadding = 7.0f;
  float detailsHeight = 108.0f;
  float detailsTilePadding = 8.0f;
  float visualHeight = 250.0f;
  float visualMinimumHeight = 236.0f;
  float visualGap = 12.0f;
  float graphFlex = 2.0f;
  float analyticsFlex = 3.0f;
  float photoPrimaryHeight = 206.0f;
  float photoSecondaryHeight = 120.0f;
  float photoGridGap = 12.0f;
};
}
```

`metricsFor(885, true)` replaces those fields with 24, 8, 184, 10, 8, 90, 4, 96, 4, 236, 236, 8, 2, 3, 196, 112, and 8 respectively. `screenContentHeight` sums header, summary, info, details, visual/legacy graph, action height, two root paddings, and five root gaps. `photoContentHeight` sums header, summary, info, details, both photo rows, their grid gap, two root paddings, and four root gaps. `photoCanvasPixelHeight` returns the larger of source drawable height and `ceil(photoContentHeight * drawableWidth / logicalWidth)`.

- [ ] **Step 4: Run the test and verify GREEN**

```bash
cmake --build cmake-build-debug --target result_layout_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^result_layout_geometry_tests$'
```

Expected: one test passes.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/scene/ResultLayoutGeometry.h tests/result_layout_geometry_tests.cpp
git commit -m "test: define responsive result layout geometry"
```

### Task 2: Live Side-by-Side Result Layout

**Files:**
- Modify: `src/skin/SkinTypes.h`
- Modify: `src/skin/DefaultSkin.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `tests/view_layout_tests.cpp`

**Interfaces:**
- Consumes: `result_layout::Metrics`, `ResultSkinData::showTimingAnalytics`, and one precomputed analytics model.
- Produces: named `resultVisuals`, `graph`, and `timingAnalytics` views with a 2:3 split.

- [ ] **Step 1: Add a failing Yoga fit test**

Build a representative 1920×885 result column in `tests/view_layout_tests.cpp` from `metricsFor(885, true)`. Give the visual row flex children of 2 and 3 and assert:

```cpp
assert(graph->getWidth() < analytics->getWidth());
assert(std::abs(static_cast<float>(graph->getWidth()) /
                    static_cast<float>(analytics->getWidth()) -
                2.0f / 3.0f) < 0.02f);
assert(actions->getY() + actions->getHeight() <= root.getHeight());
```

- [ ] **Step 2: Run the layout test and verify RED**

```bash
cmake --build cmake-build-debug --target view_layout_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^view_layout_tests$'
```

Expected: the new fit assertion fails before compact production metrics are integrated.

- [ ] **Step 3: Pass analytics availability into the skin**

Add defaulted `bool showTimingAnalytics = false;` and `bool showResultGraph = true;` fields to `ResultSkinData`. Change the scene API to:

```cpp
void addTimingAnalytics(std::optional<practice::ResultModel> analyticsModel);
```

Build the model once during `ResultScene::init()`:

```cpp
auto analyticsModel = makeTimingAnalyticsModel();
ResultSkinData data = makeResultSkinData();
data.showTimingAnalytics = analyticsModel.has_value();
skin->buildLayout("Result", rootLayout, &data);
addTimingAnalytics(std::move(analyticsModel));
```

- [ ] **Step 4: Apply responsive card metrics and build the visual row**

Resolve mobile status with `TARGET_PLATFORM == iOS || TARGET_PLATFORM == Android`. Replace the root, summary, info, details, and panel-padding literals with the policy fields. When analytics exists, build a `resultVisuals` row with metric height/min-height, gap, a graph child with flex 2, and an analytics host with flex 3. Without analytics, keep the current full-width 136-unit graph. Skip the default graph when `showResultGraph` is false.

- [ ] **Step 5: Run live layout tests and verify GREEN**

```bash
cmake --build cmake-build-debug --target result_layout_geometry_tests view_layout_tests practice_analytics_presentation_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(result_layout_geometry_tests|view_layout_tests|practice_analytics_presentation_tests)$'
```

Expected: all selected tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/skin/SkinTypes.h src/skin/DefaultSkin.cpp src/scene/ResultScene.h src/scene/ResultScene.cpp tests/view_layout_tests.cpp
git commit -m "fix: fit result controls on short mobile screens"
```

### Task 3: Two-by-Two Photo Visual Grid

**Files:**
- Modify: `src/ResultImageExporter.cpp`
- Modify: `tests/result_layout_geometry_tests.cpp`

**Interfaces:**
- Consumes: `photoVisualOrder`, compact photo heights, `showResultGraph`, and `PracticeAnalyticsView::setPhotoExportPresentation`.
- Produces: `resultPhotoVisuals` with Gauge/Histogram in row one and Lanes/Sections in row two.

- [ ] **Step 1: Strengthen the failing export geometry test**

Add these assertions:

```cpp
assert(mobile.photoPrimaryHeight == 196.0f);
assert(mobile.photoSecondaryHeight == 112.0f);
assert(mobile.photoGridGap == 8.0f);
assert(result_layout::photoContentHeight(desktop) <= 1080.0f);
assert(result_layout::photoCanvasPixelHeight(1920, 800, 1920.0f, mobile) ==
       static_cast<int>(std::ceil(result_layout::photoContentHeight(mobile))));
```

- [ ] **Step 2: Run the geometry test and verify RED**

```bash
cmake --build cmake-build-debug --target result_layout_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^result_layout_geometry_tests$'
```

Expected: the short-canvas assertion fails until the exact canvas formula is present.

- [ ] **Step 3: Build the photo grid**

In `renderResultImage`, resolve metrics from current logical height and target. Use `photoCanvasPixelHeight` instead of `kPhotoAnalyticsExtraHeight`. Set `resultSkinData.showResultGraph = !analyticsModel.has_value()`. When analytics exists, append two equal-width flex rows: a new graph placeholder beside Histogram, then Lanes beside Sections. Apply shared information only to Histogram. Point the existing gauge renderer at the new placeholder. Use `photoVisualOrder()` as the order source.

- [ ] **Step 4: Run focused tests and verify GREEN**

```bash
cmake --build cmake-build-debug --target result_layout_geometry_tests view_layout_tests practice_analytics_presentation_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(result_layout_geometry_tests|view_layout_tests|practice_analytics_presentation_tests)$'
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/ResultImageExporter.cpp tests/result_layout_geometry_tests.cpp
git commit -m "feat: arrange result photo analytics beside gauge history"
```

### Task 4: Verify and Publish

**Files:**
- Verify the approved files; do not modify replay-video layout code.

**Interfaces:**
- Consumes: all prior task outputs.
- Produces: build/test evidence and pushed `develop` commits.

- [ ] **Step 1: Run focused tests**

```bash
git diff --check
ctest --test-dir cmake-build-debug --output-on-failure -R 'result|practice_analytics|view_layout'
```

Expected: no whitespace errors and all matching tests pass.

- [ ] **Step 2: Run the repository desktop compile check**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` builds successfully.

- [ ] **Step 3: Run iOS build-only verification**

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: compile verification completes without upload. If private local setup blocks the command, record the limitation; never run the upload path.

- [ ] **Step 4: Confirm scope and push develop**

```bash
git status -sb
git diff origin/develop...HEAD --stat
gh auth status
git push origin develop
```

Expected: only the approved result layout, tests, spec, and plan are included; GitHub authentication succeeds; `origin/develop` advances to the verified local commit.
