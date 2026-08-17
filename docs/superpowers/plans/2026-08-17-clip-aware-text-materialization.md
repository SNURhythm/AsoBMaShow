# Clip-Aware Text Materialization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Defer text texture rasterization until a TextView is visible while
retaining correct Yoga measurement and row-level diagnostic history text.

**Architecture:** Keep generic `View` clipping unchanged because children may
overflow their parent. Split `TextView` measurement from its stale-texture
materialization; `renderImpl` owns the first visible materialization.

**Tech Stack:** C++23, SDL_ttf, bgfx, Yoga, Python source contracts.

## Global Constraints

- Preserve existing transformed and visible-overflow scissor behavior.
- Do not add blocking work to hidden view traversal.
- Do not whole-file format source files.

---

### Task 1: Protect deferred text materialization

**Files:**
- Modify: `tests/text_view_transient_buffer_contract_tests.py`
- Modify: `src/view/TextView.cpp`

**Interfaces:**
- Consumes: `TextView::setText`, `TextView::renderImpl`, and
  `TextView::createTexture`.
- Produces: measurement-only text updates and visible-only texture creation.

- [x] **Step 1: Write the failing contract**

Assert that the `setText` body does not call `createTexture`, while the
`renderImpl` body does.

- [x] **Step 2: Run the contract to verify it fails**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R '^text_view_transient_buffer_contract$' -j 1`

Expected: FAIL because `setText` currently creates its texture immediately.

- [x] **Step 3: Separate measurement and materialization**

Add a measurement helper that keeps `rect` current for Yoga and invalidates
the old texture. Have `renderImpl` create a missing texture only after normal
View scissor culling has admitted the view.

- [x] **Step 4: Run the contract and TextView-dependent tests**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R '^(text_view_transient_buffer_contract|view_layout_tests|play_options_panel_view_tests)$' -j 1`

Expected: PASS.

### Task 2: Preserve diagnostic row granularity

**Files:**
- Modify: `tests/settings_gameplay_skin_initialization_contract_tests.py`
- Inspect: `src/scene/SettingsSceneSkins.cpp`

**Interfaces:**
- Consumes: `snapshot.history` and `makeWrappedText`.
- Produces: one text row per `SkinDiagnosticHistoryRecord`.

- [x] **Step 1: Write a history-row contract**

Assert that the history loop calls `makeWrappedText` inside the loop rather
than constructing one text value for the entire history.

- [x] **Step 2: Run it and verify it passes**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R '^settings_gameplay_skin_initialization_contract$' -j 1`

Expected: PASS; current production code already has this intended shape.

### Task 3: Verify and publish

**Files:**
- Modify: the files from Tasks 1-2 and these plan/spec files.

- [x] **Step 1: Build and test**

Run: `cmake --build cmake-build-debug --target main -j 6`

Run: `ctest --test-dir cmake-build-debug --output-on-failure -j 6`

- [x] **Step 2: Commit and push**

Commit the implementation and tests, then push `optimize/ui-batching` to the
existing pull request.
