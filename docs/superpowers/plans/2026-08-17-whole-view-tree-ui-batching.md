# Whole-View-Tree UI Batching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Batch the ordinary `View` tree into ordered persistent dynamic-buffer submissions so standard UI rendering no longer consumes bgfx transient buffers.

**Architecture:** `ApplicationContext` owns one `UiBatchRenderer`; each `RenderContext` borrows it for a scoped view-tree traversal. The renderer appends compatible colored or textured geometry in painter order and flushes at every state boundary, then uploads each capped chunk to persistent dynamic buffers. A recording backend seam makes coalescing, boundary ordering, chunking, and failure behavior testable without a live bgfx device.

**Tech Stack:** C++23, bgfx dynamic vertex/index buffers, SDL, CMake/CTest.

## Global Constraints

- Preserve existing view painter order; do not sort UI commands.
- Do not use `bgfx::allocTransientVertexBuffer` or `bgfx::allocTransientIndexBuffer` for migrated ordinary UI paths.
- Split batches before the existing 16-bit index ceiling of 65,532 vertices or indices.
- Keep current shaders, blend/MSAA state, texture samplers, uniforms, transform matrices, and UI-logical scissors visually equivalent.
- Keep gameplay/BGA and `SkinQuadBatchRenderer` paths unchanged.
- Do not whole-file-format source files.

---

### Task 1: Add the testable persistent UI batch renderer

**Files:**
- Create: `src/rendering/UiBatchRenderer.h`
- Create: `src/rendering/UiBatchRenderer.cpp`
- Create: `tests/ui_batch_renderer_tests.cpp`
- Modify: `src/rendering/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `rendering::PosColorVertex`, `rendering::PosTexCoord0Vertex`, bgfx handles, and UI-logical scissors from `rendering/common.h`.
- Produces: `rendering::UiBatchRenderer`, `rendering::UiBatchState`, `rendering::UiBatchBackend`, and `rendering::UiBatchSubmission` for `RenderContext` and all migrated views.

- [ ] **Step 1: Write the failing focused renderer tests**

Create `tests/ui_batch_renderer_tests.cpp` with a `RecordingUiBatchBackend` that copies each submission and records its state. Define the three static layouts required by `common.h`, then add these tests:

```cpp
void testCompatibleColoredGeometryFromSeparateCallsCoalesces();
void testTextureScissorTransformProgramAndUniformChangesPreserveOrder();
void testLargeGeometrySplitsBeforeUint16IndicesWrap();
void testBackendFailureDropsOnlyThatBatchAndLaterBatchSubmits();

int main() {
  testCompatibleColoredGeometryFromSeparateCallsCoalesces();
  testTextureScissorTransformProgramAndUniformChangesPreserveOrder();
  testLargeGeometrySplitsBeforeUint16IndicesWrap();
  testBackendFailureDropsOnlyThatBatchAndLaterBatchSubmits();
  return failures == 0 ? 0 : 1;
}
```

For the first test, append two four-vertex colored quads with one identical `UiBatchState`, call `flush()`, and assert one submission with eight vertices and twelve indices. For the boundary test, append one quad for each changed field and assert the recorded order is exactly color, texture A, texture B, changed scissor, changed transform, changed program, and changed uniform. For the chunk test, append 16,384 colored quads and assert every submission has at most 65,532 vertices/indices, uses only indices smaller than its vertex count, and totals 65,536 vertices and 98,304 indices. For the failure test, configure the fake to reject its first submission, then append/flush a second distinct state and assert the second state is recorded.

- [ ] **Step 2: Register and run the new test before implementation**

Add the target beside `skin_quad_batch_renderer_tests` in `CMakeLists.txt`:

```cmake
add_executable(ui_batch_renderer_tests
    tests/ui_batch_renderer_tests.cpp
    src/rendering/UiBatchRenderer.cpp
)
target_include_directories(ui_batch_renderer_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(ui_batch_renderer_tests PRIVATE cxx_std_23)
target_link_libraries(ui_batch_renderer_tests PRIVATE ${COMMON_LIBS} bgfx)
```

Add the matching `if (TARGET ui_batch_renderer_tests)` registration beside the
other `asobmashow_register_test` calls near the end of `CMakeLists.txt`.

Run: `cmake --build cmake-build-debug --target ui_batch_renderer_tests -j 6`

Expected: the build fails because `rendering/UiBatchRenderer.h` does not exist.

- [ ] **Step 3: Define the batch protocol and recording seam**

In `src/rendering/UiBatchRenderer.h`, define a state that contains every property that can change a bgfx submission, including up to two vec4 uniforms used by the existing image-fade and shadow shaders:

```cpp
enum class UiBatchVertexFormat : std::uint8_t { Color, Textured };

struct UiBatchUniform {
  bgfx::UniformHandle handle = BGFX_INVALID_HANDLE;
  std::array<float, 4> value{};
};

struct UiBatchState {
  bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
  std::uint32_t samplerFlags = 0;
  std::uint64_t state = 0;
  std::optional<UiLogicalRect> scissor;
  std::optional<std::array<float, 16>> transform;
  std::array<UiBatchUniform, 2> uniforms{};
  std::size_t uniformCount = 0;
};

struct UiBatchSubmission {
  UiBatchVertexFormat format;
  std::span<const PosColorVertex> colorVertices;
  std::span<const PosTexCoord0Vertex> texturedVertices;
  std::span<const std::uint16_t> indices;
  UiBatchState state;
};

class UiBatchBackend {
public:
  virtual ~UiBatchBackend() = default;
  virtual bool submit(const UiBatchSubmission &) noexcept = 0;
};
```

Declare `appendColor`, `appendTextured`, `flush`, `begin`, `end`, and `discard` on `UiBatchRenderer`. The append functions must copy incoming geometry, rebase incoming indices, flush before a non-equal state, and flush before either stored array would exceed 65,532 elements. State equality must compare every handle index, scalar, optional scissor, optional matrix, and active uniform value.

- [ ] **Step 4: Implement dynamic-buffer production submission**

Implement `BgfxUiBatchBackend` privately in `UiBatchRenderer.cpp`. It owns one dynamic vertex buffer per `UiBatchVertexFormat` and one dynamic index buffer, all created with `BGFX_BUFFER_ALLOW_RESIZE`. Its `submit` implementation must update the selected buffers with `bgfx::copy`, bind the submitted subranges, bind active uniforms and the optional sampler texture, apply the UI scissor through `setScissorUI`, set the optional transform, set the submitted state, and submit to `rendering::ui_view`.

```cpp
bgfx::update(vertexBuffer, 0,
             bgfx::copy(vertices.data(), vertices.size_bytes()));
bgfx::update(indexBuffer, 0,
             bgfx::copy(indices.data(), indices.size_bytes()));
bgfx::setVertexBuffer(0, vertexBuffer, 0,
                      static_cast<std::uint32_t>(vertices.size()));
bgfx::setIndexBuffer(indexBuffer, 0,
                     static_cast<std::uint32_t>(indices.size()));
bgfx::submit(rendering::ui_view, state.program);
```

Return `false` when buffer creation or a required program/texture handle is invalid, log a rate-limited diagnostic in the renderer, and clear the failed CPU chunk. Never call a transient-buffer API from this implementation. Destroy every valid dynamic handle in `UiBatchRenderer::shutdown()` and make shutdown idempotent.

- [ ] **Step 5: Make the focused tests pass and commit**

Run: `cmake --build cmake-build-debug --target ui_batch_renderer_tests -j 6 && ctest --test-dir cmake-build-debug --output-on-failure -R '^ui_batch_renderer_tests$'`

Expected: all four tests pass.

Commit:

```bash
git add src/rendering/UiBatchRenderer.h src/rendering/UiBatchRenderer.cpp \
        tests/ui_batch_renderer_tests.cpp src/rendering/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add persistent UI batch renderer"
```

### Task 2: Bind UI batching to application and view-tree lifetimes

**Files:**
- Modify: `src/context.h`
- Modify: `src/view/View.h`
- Modify: `src/scene/Scene.h`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/ui_batch_renderer_tests.cpp`

**Interfaces:**
- Consumes: `UiBatchRenderer` from Task 1 and existing `ApplicationContext`/`RenderContext` lifetimes.
- Produces: `RenderContext::UiBatchScope`, `RenderContext::flushUiBatch()`, and an application-owned `uiBatchRenderer` used by ordinary scene view traversals.

- [ ] **Step 1: Add failing scope and ordering tests**

Extend `tests/ui_batch_renderer_tests.cpp` with a fake-backed `RenderContext` scope test that queues a colored quad before a custom boundary, calls `flushUiBatch()`, queues another compatible quad, and ends the scope. Assert two submissions in that order; the custom boundary must not be merged with either side. Add a nested scope assertion showing that the inner scope does not call `begin()` or `end()` on the shared renderer.

- [ ] **Step 2: Run the focused test and verify the missing scope API**

Run: `cmake --build cmake-build-debug --target ui_batch_renderer_tests -j 6`

Expected: compilation fails because `RenderContext::UiBatchScope` and `flushUiBatch()` are undeclared.

- [ ] **Step 3: Add persistent ownership and scoped render-context access**

Add `rendering::UiBatchRenderer uiBatchRenderer;` to `ApplicationContext` and
call `uiBatchRenderer.shutdown()` at the start of `ApplicationContext`'s
destructor. That destructor runs when `run()` returns, before its caller
releases uniforms and calls `bgfx::shutdown()`. Extend `RenderContext` with an
optional `UiBatchRenderer *`, a constructor accepting `UiBatchRenderer &`,
`appendUiColor`, `appendUiTextured`, `flushUiBatch()`, and this nested RAII
scope:

```cpp
struct UiBatchScope {
  explicit UiBatchScope(RenderContext &context) : context(context) {
    if (context.uiBatchDepth++ == 0 && context.uiBatch != nullptr) {
      context.uiBatch->begin();
    }
  }
  ~UiBatchScope() {
    if (--context.uiBatchDepth == 0 && context.uiBatch != nullptr) {
      context.uiBatch->end();
    }
  }
  RenderContext &context;
};
```

`flushUiBatch()` must call `uiBatch->flush()` only when the pointer is non-null. The default `RenderContext` remains valid for headless tests and disables queueing rather than creating GPU resources.
`appendUiColor` and `appendUiTextured` must similarly return `false` without
queueing when no batcher was supplied, while production render contexts always
receive the application-owned batcher.

- [ ] **Step 4: Scope every normal Scene traversal around scene boundaries**

Change `Scene::render()` to construct `RenderContext renderContext(context.uiBatchRenderer)`, open one `RenderContext::UiBatchScope`, flush immediately before `renderScene()`, and retain the same scope for views rendered after `renderScene()`:

```cpp
RenderContext renderContext(context.uiBatchRenderer);
RenderContext::UiBatchScope uiBatchScope(renderContext);
renderViewsBeforeScene(renderContext);
renderContext.flushUiBatch();
renderScene();
renderViewsAfterScene(renderContext);
```

Keep the two existing loops inline if preferred; preserve their filtering and order exactly. Where non-`Scene` production render passes construct a `RenderContext` and render a `View` (`GamePlayScene`, `ChartViewerScene`, `ReplayVideoExporter`, and `ResultImageExporter`), pass the owning `ApplicationContext::uiBatchRenderer` and wrap the related root renders in `UiBatchScope`.

- [ ] **Step 5: Link renderer code into all direct View test targets and verify**

In `src/rendering/CMakeLists.txt`, add `UiBatchRenderer.cpp` to `main`. In the test-only CMake section, add `src/rendering/UiBatchRenderer.cpp` to each target that directly compiles `src/view/View.cpp`: `builtin_renderer_characterization_tests`, `ir_ranking_modal_tests`, `image_view_fade_tests`, `chart_list_item_view_tests`, `context_menu_view_tests`, `view_layout_tests`, `button_enabled_tests`, `text_input_box_tests`, `checkbox_button_content_tests`, `play_options_panel_view_tests`, `replay_summary_list_view_tests`, `result_record_list_view_tests`, `ir_upload_candidate_list_view_tests`, `replay_playfield_presentation_tests`, `result_presentation_model_tests`, and `result_image_exporter_partial_tests`.

Run: `cmake --build cmake-build-debug --target ui_batch_renderer_tests view_layout_tests main -j 6 && ctest --test-dir cmake-build-debug --output-on-failure -R '^(ui_batch_renderer_tests|view_layout_tests)$'`

Expected: all selected targets build and both tests pass.

Commit:

```bash
git add src/context.h src/view/View.h src/scene/Scene.h src/main.cpp \
        src/rendering/CMakeLists.txt CMakeLists.txt tests/ui_batch_renderer_tests.cpp \
        src/scene/play/GamePlayScene.cpp src/scene/ChartViewerScene.cpp \
        src/ReplayVideoExporter.cpp src/ResultImageExporter.cpp
git commit -m "feat: scope UI batching across view trees"
```

### Task 3: Migrate all colored view-tree geometry

**Files:**
- Modify: `src/view/View.cpp`
- Modify: `src/view/View.h`
- Modify: `src/view/Button.cpp`
- Modify: `src/view/TextInputBox.cpp`
- Modify: `src/view/RecyclerView.h`
- Modify: `src/view/ScrollView.cpp`
- Modify: `src/view/SnappedSlider.h`
- Modify: `src/view/SnappedSlider.cpp`
- Create: `tests/ui_batching_colored_contract_tests.py`
- Test: `tests/ui_batch_renderer_tests.cpp`

**Interfaces:**
- Consumes: Task 1 append methods and Task 2 scope/flush access.
- Produces: colored UI fills, gradients, borders, triangle-fan rounded rectangles, shadow quads, selections, carets, and scroll controls with no per-widget transient allocations.

- [ ] **Step 1: Write failing geometry-equivalence and colored-path contract tests**

Add tests that append the existing rectangle indices `{0, 1, 2, 0, 2, 3}` and a rounded-rectangle fan through `UiBatchRenderer`. Assert their copied vertices, colors, and indices equal the previous immediate-mode geometry. Add a transform/scissor case and assert they are retained on the captured batch state.

Create `tests/ui_batching_colored_contract_tests.py`. It must scan
`View.cpp`, `View.h`, `Button.cpp`, `TextInputBox.cpp`, `RecyclerView.h`,
`ScrollView.cpp`, and `SnappedSlider.cpp`, fail when any of them contains
`allocTransientVertexBuffer`, `allocTransientIndexBuffer`,
`getAvailTransientVertexBuffer`, or `getAvailTransientIndexBuffer`, and
require `appendUiColor` in each migrated rendering implementation.

- [ ] **Step 2: Run the new tests to prove the shape helpers do not exist**

Run: `cmake --build cmake-build-debug --target ui_batch_renderer_tests -j 6 && python3 -m unittest tests/ui_batching_colored_contract_tests.py -v`

Expected: the C++ build fails because the color geometry helper calls are
absent, and the Python contract fails because the listed files still allocate
transient buffers.

- [ ] **Step 3: Replace `View` and `Button` immediate colored submissions**

Replace `submitColoredRect`, `submitRoundedRect`, and `submitGradientRect` in `View.cpp`, the DEBUG bounding-box submission in `View.h`, and `drawButtonRect` in `Button.cpp` with geometry construction followed by `context.appendUiColor(vertices, indices, state)`. Build state with `SHADER_SIMPLE`, the existing `BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA` flags (plus `BGFX_STATE_MSAA` where it was present), the existing UI scissor, and `context.getTransformMatrix()` when non-null. Keep shadow geometry textured-format with `SHADER_UI_SHADOW`, its two vec4 uniforms, and its existing state.

- [ ] **Step 4: Replace text-input and scrolling transient colored paths**

Change `TextInputBox::submitRect`, `RecyclerView::drawScrollbarRect`, `ScrollView::renderPersistentScrollbar`, and `SnappedSlider::renderImpl` to append their current rectangle/rounded-rectangle geometry to the context batcher. Remove the `SimpleBatchRenderer` member from `SnappedSlider` and the local `SimpleBatchRenderer` from `ScrollView`; retain their colors, radii, clipping, and transform behavior unchanged.

- [ ] **Step 5: Verify colored UI paths and commit**

Run: `cmake --build cmake-build-debug --target ui_batch_renderer_tests button_enabled_tests text_input_box_tests play_options_panel_view_tests -j 6 && python3 -m unittest tests/ui_batching_colored_contract_tests.py -v && ctest --test-dir cmake-build-debug --output-on-failure -R '^(ui_batch_renderer_tests|button_enabled_tests|text_input_box_tests|play_options_panel_view_tests)$'`

Expected: selected tests pass, and `rg -n 'allocTransient|createRect\(' src/view/View.cpp src/view/View.h src/view/Button.cpp src/view/TextInputBox.cpp src/view/RecyclerView.h src/view/ScrollView.cpp src/view/SnappedSlider.cpp` reports no render-path transient allocation call.

Commit:

```bash
git add src/view/View.cpp src/view/View.h src/view/Button.cpp \
        src/view/TextInputBox.cpp src/view/RecyclerView.h src/view/ScrollView.cpp \
        src/view/SnappedSlider.h src/view/SnappedSlider.cpp \
        tests/ui_batch_renderer_tests.cpp tests/ui_batching_colored_contract_tests.py
git commit -m "refactor: batch colored view tree geometry"
```

### Task 4: Migrate text and image view-tree geometry

**Files:**
- Modify: `src/view/TextView.cpp`
- Modify: `src/view/ImageView.cpp`
- Modify: `tests/text_view_transient_buffer_contract_tests.py`
- Create: `tests/ui_batching_textured_contract_tests.py`
- Modify: `tests/ui_batch_renderer_tests.cpp`
- Test: `tests/image_view_fade_tests.cpp`

**Interfaces:**
- Consumes: textured `UiBatchRenderer::appendTextured` and uniform-aware `UiBatchState` from Task 1.
- Produces: batched text and image submissions that preserve UV order, clip, texture, fade, scrim, and rounded-corner behavior.

- [ ] **Step 1: Write failing textured-state tests**

Extend `tests/ui_batch_renderer_tests.cpp` with one normal text state and two image-fade states that share a texture but have different `u_imageFadeParams`. Assert normal text quads with matching texture/scissor/transform coalesce, while the two image-fade states produce two submissions because their uniform values differ. Assert the vertex order is `{left, top}, {right, top}, {right, bottom}, {left, bottom}` with the current UV convention.

- [ ] **Step 2: Update the transient contracts to demand batching and run them red**

Replace `test_text_submission_checks_transient_capacity_before_allocating` in `tests/text_view_transient_buffer_contract_tests.py` with assertions that `TextView.cpp` calls `context.appendUiTextured` and does not contain `allocTransientVertexBuffer`, `allocTransientIndexBuffer`, `getAvailTransientVertexBuffer`, or `getAvailTransientIndexBuffer` inside `renderImpl`.

Create `tests/ui_batching_textured_contract_tests.py` with the same forbidden
API scan for `TextView.cpp` and `ImageView.cpp`; it must require
`appendUiTextured` in both files.

Run: `python3 -m unittest tests/text_view_transient_buffer_contract_tests.py -v && python3 -m unittest tests/ui_batching_textured_contract_tests.py -v`

Expected: FAIL because `TextView::renderImpl` and `ImageView`'s rounded-image
helper still allocate transient buffers.

- [ ] **Step 3: Route `TextView` through the textured batcher**

Replace the `submitText` transient allocation with four `PosTexCoord0Vertex` values and the current six indices. Queue them through `context.appendUiTextured` with `SHADER_TEXT`, `s_texColor`, the text texture, alpha blend state, optional transform, and current UI scissor. Preserve the marquee and overflow scopes exactly; they must only change the state key, not geometry order.

- [ ] **Step 4: Route `ImageView` through the textured batcher**

Replace both branches of `submitTexturedRoundedRect` with the same quad/fan vertices and indices queued through `context.appendUiTextured`. Populate `UiBatchState` with the chosen program, image texture, sampler, existing blend/MSAA state, scissor, transform, and—when fade or scrim is active—the exact `u_imageFadeParams` and `u_imageScrimColor` values already calculated in `renderImpl`.

- [ ] **Step 5: Verify textured behavior and commit**

Run: `cmake --build cmake-build-debug --target ui_batch_renderer_tests image_view_fade_tests chart_list_item_view_tests -j 6 && python3 -m unittest tests/text_view_transient_buffer_contract_tests.py -v && python3 -m unittest tests/ui_batching_textured_contract_tests.py -v && ctest --test-dir cmake-build-debug --output-on-failure -R '^(ui_batch_renderer_tests|image_view_fade_tests|chart_list_item_view_tests)$'`

Expected: all commands pass; the Python contract proves text no longer allocates transient buffers.

Commit:

```bash
git add src/view/TextView.cpp src/view/ImageView.cpp \
        tests/text_view_transient_buffer_contract_tests.py \
        tests/ui_batching_textured_contract_tests.py tests/ui_batch_renderer_tests.cpp
git commit -m "refactor: batch textured view tree geometry"
```

### Task 5: Audit direct UI boundaries and run release-grade verification

**Files:**
- Modify: `docs/superpowers/specs/2026-08-17-whole-view-tree-ui-batching-design.md`
- Test: `tests/ui_batch_renderer_tests.cpp`
- Test: `tests/ui_batching_colored_contract_tests.py`
- Test: `tests/ui_batching_textured_contract_tests.py`

**Interfaces:**
- Consumes: completed dynamic batching and migrated view paths.
- Produces: explicit direct-render flush boundaries and verification evidence for all ordinary UI paths.

- [ ] **Step 1: Add explicit flushes around remaining direct UI submissions**

For every remaining direct bgfx submission reachable during normal view rendering, call `context.flushUiBatch()` immediately before setting direct bgfx state and again after it submits. Retain direct calls only for non-view specialized rendering; record each retained path under the spec's “View integrations” section as an intentional boundary.

- [ ] **Step 2: Run targeted and full verification**

Run:

```bash
cmake --build cmake-build-debug --target main ui_batch_renderer_tests \
    view_layout_tests text_input_box_tests image_view_fade_tests \
    play_options_panel_view_tests -j 6
python3 -m unittest tests/text_view_transient_buffer_contract_tests.py -v
python3 -m unittest tests/ui_batching_colored_contract_tests.py -v
python3 -m unittest tests/ui_batching_textured_contract_tests.py -v
ctest --test-dir cmake-build-debug --output-on-failure -j 6
git diff --check
```

Expected: every command succeeds. The full CTest run has no UI batching regression, and `git diff --check` emits no whitespace errors.

- [ ] **Step 3: Commit the audited implementation**

```bash
git add docs/superpowers/specs/2026-08-17-whole-view-tree-ui-batching-design.md \
        tests/ui_batching_colored_contract_tests.py \
        tests/ui_batching_textured_contract_tests.py \
        tests/text_view_transient_buffer_contract_tests.py tests/ui_batch_renderer_tests.cpp
git commit -m "test: verify whole-tree UI batching"
```
