# Start Lane Indicator Layering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep start-lane triangles visible above an overlapping lane cover and allow the lane cover to reach 100%.

**Architecture:** Keep the existing cover-following triangle geometry. Move lane-cover and indicator depths into the geometry contract so the focused test enforces that indicators render above the cover, then use those depths in `BMSRenderer`. Raise the shared `AppSettings` maximum so persistence, controls, dragging, replay playback, and export inherit the same 100% range.

**Tech Stack:** C++23, bgfx batch rendering, AppSettings sanitization, CMake/CTest.

## Global Constraints

- Triangles continue cascading with the lane-cover edge and retain the existing gap while space permits.
- When cover and triangle geometry overlap, the triangle renders above the cover.
- The lane-cover maximum is 100% in settings, dragging, replay, and export.
- Existing saved lane-cover values remain unchanged.
- Work on the current `feature/show-starting-lane` branch as requested; do not create another worktree.

---

### Task 1: Enforce Indicator-Above-Cover Layering

**Files:**
- Modify: `tests/start_lane_indicator_geometry_tests.cpp`
- Modify: `src/scene/play/StartLaneIndicatorGeometry.h`
- Modify: `src/scene/play/BMSRenderer.cpp`

**Interfaces:**
- Consumes: existing `start_lane_indicator::placeTriangle` geometry.
- Produces: `start_lane_indicator::kLaneCoverDepth` and `start_lane_indicator::kIndicatorDepth`, with indicator depth strictly greater than cover depth.

- [ ] **Step 1: Write the failing layer-order test**

Add to `tests/start_lane_indicator_geometry_tests.cpp`:

```cpp
check(kIndicatorDepth > kLaneCoverDepth,
      "overlapping triangle renders above the lane cover");
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target start_lane_indicator_geometry_tests -j 6
```

Expected: compilation fails because `kIndicatorDepth` and `kLaneCoverDepth` do not exist.

- [ ] **Step 3: Add the depth contract and use it in the renderer**

Add to `StartLaneIndicatorGeometry.h`:

```cpp
inline constexpr uint32_t kLaneCoverDepth = 300;
inline constexpr uint32_t kIndicatorDepth = 320;
```

Include `<cstdint>`. Remove the local indicator and cover depth constants from `BMSRenderer::render`, draw the lane-cover pass first using `kLaneCoverDepth`, and draw the start-lane indicator pass second using `kIndicatorDepth`. Leave the geometry and gap calculation unchanged.

- [ ] **Step 4: Run the focused test and renderer build**

Run:

```bash
cmake --build cmake-build-debug --target start_lane_indicator_geometry_tests main -j 6
./cmake-build-debug/start_lane_indicator_geometry_tests
```

Expected: build and test exit `0`.

### Task 2: Raise Lane-Cover Maximum to 100%

**Files:**
- Modify: `tests/audio_video_settings_tests.cpp`
- Modify: `src/AppSettings.h`

**Interfaces:**
- Consumes: `AppSettings::sanitize` and all existing clamps that reference `kMaxNoteStartPositionPercent`.
- Produces: a shared 100% maximum for persistence, settings controls, renderer state, dragging, replay playback, and export.

- [ ] **Step 1: Write the failing settings-boundary test**

In `appSettingsSanitizesAudioVideoSettings`, set and assert the new boundary:

```cpp
value.noteStartPositionPercent = 100;
value.sanitize();
ASSERT_TRUE(value.noteStartPositionPercent == 100,
            "AppSettings accepts full lane cover");
```

Also verify values above the range clamp:

```cpp
value.noteStartPositionPercent = 101;
value.sanitize();
ASSERT_TRUE(value.noteStartPositionPercent == 100,
            "AppSettings clamps lane cover above 100 percent");
```

- [ ] **Step 2: Run the settings test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target audio_video_settings_tests -j 6
./cmake-build-debug/audio_video_settings_tests
```

Expected: the 100% assertion fails because the current maximum is 90%.

- [ ] **Step 3: Raise the shared maximum**

Change `src/AppSettings.h`:

```cpp
static constexpr int kMaxNoteStartPositionPercent = 100;
```

No call-site-specific clamp is added; existing consumers continue using the shared constant.

- [ ] **Step 4: Run focused verification**

Run:

```bash
cmake --build cmake-build-debug --target audio_video_settings_tests \
  start_lane_indicator_geometry_tests main -j 6
./cmake-build-debug/audio_video_settings_tests
./cmake-build-debug/start_lane_indicator_geometry_tests
```

Expected: builds and tests exit `0`.

### Task 3: Full Verification and Commit

**Files:**
- Modify only if verification exposes a defect in the files listed above.

**Interfaces:**
- Consumes: completed layering and range changes.
- Produces: a verified, committed branch.

- [ ] **Step 1: Run the complete desktop build and test suite**

```bash
cmake --build cmake-build-debug -j 6
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: build exits `0`; all CTest tests pass.

- [ ] **Step 2: Inspect and commit**

```bash
git diff --check
git status --short
git add src/AppSettings.h src/scene/play/BMSRenderer.cpp \
  src/scene/play/StartLaneIndicatorGeometry.h \
  tests/audio_video_settings_tests.cpp \
  tests/start_lane_indicator_geometry_tests.cpp
git commit -m "fix: render start indicators above lane cover"
```

Expected: no whitespace errors; only the intended implementation and test files are committed.
