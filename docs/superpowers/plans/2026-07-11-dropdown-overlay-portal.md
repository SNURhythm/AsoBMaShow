# Dropdown Overlay Portal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render settings dropdown menus through a reusable scene-root overlay portal so scrolling containers cannot clip them or paint later controls above them.

**Architecture:** `OverlayPortal` is a generic, non-owning root-layer presenter for arbitrary `View` instances. `DropdownView` keeps ownership of its menu, registers it with the portal only while open, and calculates an anchored placement against window bounds. Settings owns one portal below blocking modals and passes it to every settings dropdown.

**Tech Stack:** C++23, Yoga layout, SDL events, existing `View`/`ScrollView` renderer, CTest.

## Global Constraints

- Name the reusable component `OverlayPortal`; do not use a dropdown-specific host name.
- The portal must not own presented views; presenters retain lifecycle responsibility.
- Dropdown placement considers window edges, not scrolling-container edges.
- Existing dropdown callers without a portal retain their local-menu behavior.
- Settings blocking overlays at z-index 1000+ remain above dropdown menus.

---

### Task 1: Generic overlay portal and anchored placement

**Files:**
- Create: `src/view/OverlayPortal.h`
- Modify: `tests/dropdown_view_tests.cpp`
- Modify: `tests/view_layout_tests.cpp`

**Interfaces:**
- Produces: `OverlayPlacement placeAnchoredOverlay(const OverlayAnchor &, int desiredWidth, int desiredHeight, int minimumHeight, int viewportWidth, int viewportHeight, int margin, int gap)`
- Produces: `OverlayPortal::present(View *)`, `OverlayPortal::dismiss(View *)`, and `OverlayPortal::isPresented(const View *) const`

- [ ] **Step 1: Write failing placement and portal tests**

Add placement assertions for a centered anchor opening below, a bottom anchor
flipping above, a right-edge anchor shifting left, and a constrained window
clamping height. Add a `view_layout_tests` event test showing a presented view
receives events before background content and stops receiving them after
`dismiss`.

- [ ] **Step 2: Run RED**

Run:

```bash
cmake --build cmake-build-debug --target dropdown_view_tests view_layout_tests -j 6
```

Expected: compilation fails because `OverlayPortal.h` and its interfaces do
not exist.

- [ ] **Step 3: Implement the generic portal**

Create a header-only `OverlayPortal` that stores a deduplicated non-owning
stack of presented views, renders in presentation order, dispatches events in
reverse order, and propagates themes to presented views. Implement the pure
anchored-placement helper with below/above selection and window clamping.

- [ ] **Step 4: Run GREEN**

Run:

```bash
cmake --build cmake-build-debug --target dropdown_view_tests view_layout_tests -j 6
./cmake-build-debug/dropdown_view_tests
./cmake-build-debug/view_layout_tests
```

Expected: both targets pass.

- [ ] **Step 5: Commit**

```bash
git add src/view/OverlayPortal.h tests/dropdown_view_tests.cpp tests/view_layout_tests.cpp
git commit -m "feat: add reusable overlay portal"
```

### Task 2: Portal-backed dropdown menus in Settings

**Files:**
- Modify: `src/view/DropdownView.h`
- Modify: `src/view/DropdownView.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneAudioVideo.cpp`
- Modify: `src/scene/SettingsSceneInput.cpp`

**Interfaces:**
- Consumes: `OverlayPortal`, `placeAnchoredOverlay`
- Produces: `DropdownView(Callbacks callbacks, OverlayPortal *portal = nullptr)`

- [ ] **Step 1: Extend the failing dropdown contract**

Add compile-time checks that `DropdownView` accepts an optional
`OverlayPortal *`, retains a portal pointer, and overrides movement so an open
menu tracks a scrolled trigger. The placement tests from Task 1 remain the
behavioral contract for edge dodging.

- [ ] **Step 2: Run RED**

Run:

```bash
cmake --build cmake-build-debug --target dropdown_view_tests -j 6
```

Expected: compilation fails because the portal-backed constructor and state do
not exist.

- [ ] **Step 3: Move open-menu rendering into the portal**

When a portal is supplied, keep `menuScroll` outside the dropdown child tree,
present it only while open, position it in absolute window coordinates using
`placeAnchoredOverlay`, update placement from both `onLayout` and `onMove`, and
dismiss/delete it safely in the dropdown destructor. Preserve the existing
local child path when no portal is supplied.

- [ ] **Step 4: Wire one Settings portal**

Create `OverlayPortal *overlayPortal` during settings layout construction,
pass it to all audio, display, and input dropdowns, add it to `rootLayout`
after normal content at z-index 900, and clear its member pointer during scene
reset. Keep conflict/import/display blocking overlays at their current higher
z-indices.

- [ ] **Step 5: Run GREEN and compile the app**

Run:

```bash
cmake --build cmake-build-debug --target dropdown_view_tests view_layout_tests main -j 6
./cmake-build-debug/dropdown_view_tests
./cmake-build-debug/view_layout_tests
```

Expected: tests pass and `main` links successfully.

- [ ] **Step 6: Commit**

```bash
git add src/view/DropdownView.h src/view/DropdownView.cpp src/scene/SettingsScene.h src/scene/SettingsSceneLayout.cpp src/scene/SettingsSceneAudioVideo.cpp src/scene/SettingsSceneInput.cpp tests/dropdown_view_tests.cpp
git commit -m "fix: render settings dropdowns in overlay portal"
```
