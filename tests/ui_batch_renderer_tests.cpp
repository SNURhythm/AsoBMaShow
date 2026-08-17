#include "rendering/UiBatchRenderer.h"
#include "view/View.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace rendering {
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
float near_clip = -1.0F;
float far_clip = 1.0F;
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
} // namespace rendering

namespace {

int failures = 0;

void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct CapturedSubmission {
  std::vector<rendering::PosColorVertex> vertices;
  std::vector<std::uint16_t> indices;
  rendering::UiBatchState state;
};

struct RecordingBackend final : rendering::UiBatchBackend {
  bool submit(const rendering::UiBatchSubmission &submission) noexcept override {
    if (rejectedSubmissions != 0) {
      --rejectedSubmissions;
      return false;
    }
    CapturedSubmission captured;
    captured.vertices.assign(submission.colorVertices.begin(),
                             submission.colorVertices.end());
    captured.indices.assign(submission.indices.begin(), submission.indices.end());
    captured.state = submission.state;
    submissions.push_back(std::move(captured));
    return true;
  }

  std::vector<CapturedSubmission> submissions;
  std::size_t rejectedSubmissions = 0;
};

rendering::UiBatchState colorState() {
  return {.program = bgfx::ProgramHandle{7},
          .state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_ALPHA};
}

constexpr std::array<rendering::PosColorVertex, 4> kFirstQuad = {
    rendering::PosColorVertex{0.0F, 0.0F, 0.0F, 0xff0000ffU},
    rendering::PosColorVertex{10.0F, 0.0F, 0.0F, 0xff0000ffU},
    rendering::PosColorVertex{10.0F, 10.0F, 0.0F, 0xff0000ffU},
    rendering::PosColorVertex{0.0F, 10.0F, 0.0F, 0xff0000ffU},
};

constexpr std::array<rendering::PosColorVertex, 4> kSecondQuad = {
    rendering::PosColorVertex{20.0F, 0.0F, 0.0F, 0x00ff00ffU},
    rendering::PosColorVertex{30.0F, 0.0F, 0.0F, 0x00ff00ffU},
    rendering::PosColorVertex{30.0F, 10.0F, 0.0F, 0x00ff00ffU},
    rendering::PosColorVertex{20.0F, 10.0F, 0.0F, 0x00ff00ffU},
};

constexpr std::array<std::uint16_t, 6> kQuadIndices = {0, 1, 2, 0, 2, 3};

class DirectUiBoundaryProbeView final : public View {
public:
  explicit DirectUiBoundaryProbeView(RecordingBackend &backend)
      : backend(backend) {}

  [[nodiscard]] bool sawQueuedUiBeforeCustomDraw() const noexcept {
    return sawQueuedUiBeforeCustomDraw_;
  }

protected:
  [[nodiscard]] bool requiresUiBatchBoundary() const noexcept override {
    return true;
  }

  void renderImpl(RenderContext &context) override {
    sawQueuedUiBeforeCustomDraw_ = backend.submissions.size() == 1;
    context.appendUiColor(kSecondQuad, kQuadIndices, colorState());
  }

private:
  RecordingBackend &backend;
  bool sawQueuedUiBeforeCustomDraw_ = false;
};

class QueuedUiParentView final : public View {
protected:
  void renderImpl(RenderContext &context) override {
    context.appendUiColor(kFirstQuad, kQuadIndices, colorState());
  }
};

void testCompatibleColoredGeometryCoalesces() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);

  renderer.begin();
  renderer.appendColor(kFirstQuad, kQuadIndices, colorState());
  renderer.appendColor(kSecondQuad, kQuadIndices, colorState());
  renderer.flush();

  expect(backend.submissions.size() == 1,
         "adjacent compatible colored UI quads emit one submission");
  if (backend.submissions.size() != 1) {
    return;
  }
  const auto &submission = backend.submissions.front();
  expect(submission.vertices.size() == 8,
         "coalesced colored submission keeps every vertex");
  expect(submission.indices ==
             std::vector<std::uint16_t>({0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7}),
         "coalesced colored submission rebases second-quad indices");
  expect(submission.vertices[4].x == 20.0F &&
             submission.vertices[7].abgr == 0x00ff00ffU,
         "coalesced colored submission preserves painter-order geometry");
}

void testRenderContextScopeFlushesOnlyAtExplicitBoundary() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);
  RenderContext context(renderer);

  {
    RenderContext::UiBatchScope outer(context);
    context.appendUiColor(kFirstQuad, kQuadIndices, colorState());
    {
      RenderContext::UiBatchScope inner(context);
      context.appendUiColor(kSecondQuad, kQuadIndices, colorState());
    }
    expect(backend.submissions.empty(),
           "nested view traversal keeps compatible UI geometry queued");

    context.flushUiBatch();
    context.appendUiColor(kFirstQuad, kQuadIndices, colorState());
  }

  expect(backend.submissions.size() == 2,
         "an explicit UI boundary splits otherwise compatible submissions");
  if (backend.submissions.size() != 2) {
    return;
  }
  expect(backend.submissions[0].vertices.size() == 8 &&
             backend.submissions[1].vertices.size() == 4,
         "outer scope preserves ordered geometry across an explicit boundary");
}

void testDirectViewFlushesDecorationsBeforeAndUiAfterCustomDraw() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);
  RenderContext context(renderer);
  QueuedUiParentView parent;
  auto *view = new DirectUiBoundaryProbeView(backend);
  parent.addView(view);
  parent.setSize(32, 24);
  view->setSize(32, 24);

  {
    RenderContext::UiBatchScope scope(context);
    parent.render(context);
  }

  expect(view->sawQueuedUiBeforeCustomDraw(),
         "a direct view observes queued UI before custom drawing");
  expect(backend.submissions.size() == 2,
         "a direct view flushes UI queued after custom drawing before later views");
}

void testProgramBoundaryPreservesSubmissionOrder() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);
  auto firstState = colorState();
  auto secondState = firstState;
  secondState.program = bgfx::ProgramHandle{8};

  renderer.begin();
  renderer.appendColor(kFirstQuad, kQuadIndices, firstState);
  renderer.appendColor(kSecondQuad, kQuadIndices, secondState);
  renderer.end();

  expect(backend.submissions.size() == 2,
         "a changed UI program emits an ordered batch boundary");
  if (backend.submissions.size() != 2) {
    return;
  }
  expect(backend.submissions[0].state.program.idx == 7 &&
             backend.submissions[0].vertices.front().x == 0.0F &&
             backend.submissions[1].state.program.idx == 8 &&
             backend.submissions[1].vertices.front().x == 20.0F,
         "a UI program boundary retains each side's painter order");
}

void testStateBoundariesPreserveSubmissionOrder() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);
  auto state = colorState();

  renderer.begin();
  renderer.appendColor(kFirstQuad, kQuadIndices, state);

  state.scissor = {.x = 10, .y = 20, .width = 30, .height = 40};
  renderer.appendColor(kSecondQuad, kQuadIndices, state);

  state.transform = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                     0.0F, 0.0F, 1.0F, 0.0F, 5.0F, 6.0F, 0.0F, 1.0F};
  renderer.appendColor(kFirstQuad, kQuadIndices, state);

  state.uniforms[0] = {.handle = bgfx::UniformHandle{3},
                       .value = {1.0F, 2.0F, 3.0F, 4.0F}};
  state.uniformCount = 1;
  renderer.appendColor(kSecondQuad, kQuadIndices, state);

  state.texture = bgfx::TextureHandle{5};
  state.sampler = bgfx::UniformHandle{6};
  renderer.appendColor(kFirstQuad, kQuadIndices, state);
  renderer.end();

  expect(backend.submissions.size() == 5,
         "scissor, transform, uniform, and texture changes split UI batches");
  if (backend.submissions.size() != 5) {
    return;
  }
  expect(backend.submissions[0].vertices.front().x == 0.0F &&
             backend.submissions[1].vertices.front().x == 20.0F &&
             backend.submissions[2].vertices.front().x == 0.0F &&
             backend.submissions[3].vertices.front().x == 20.0F &&
             backend.submissions[4].vertices.front().x == 0.0F,
         "every UI state boundary preserves painter order");
}

void testBatchSplitsBeforeUint16IndexLimit() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);

  renderer.begin();
  for (std::size_t index = 0; index < 16'384; ++index) {
    renderer.appendColor(kFirstQuad, kQuadIndices, colorState());
  }
  renderer.end();

  expect(backend.submissions.size() == 2,
         "a large UI run splits before 16-bit indices wrap");
  std::size_t vertexCount = 0;
  std::size_t indexCount = 0;
  bool validIndices = true;
  for (const auto &submission : backend.submissions) {
    vertexCount += submission.vertices.size();
    indexCount += submission.indices.size();
    validIndices = validIndices &&
                   submission.vertices.size() <=
                       rendering::UiBatchRenderer::kMaximumVertices &&
                   submission.indices.size() <=
                       rendering::UiBatchRenderer::kMaximumIndices;
    for (const auto index : submission.indices) {
      validIndices = validIndices && index < submission.vertices.size();
    }
  }
  expect(vertexCount == 65'536 && indexCount == 98'304 && validIndices,
         "split UI batches retain every quad without index corruption");
}

void testFailedBatchDoesNotBlockLaterSubmission() {
  RecordingBackend backend;
  backend.rejectedSubmissions = 1;
  rendering::UiBatchRenderer renderer(backend);

  renderer.begin();
  renderer.appendColor(kFirstQuad, kQuadIndices, colorState());
  expect(!renderer.flush(), "a rejected dynamic upload reports failure");
  renderer.appendColor(kSecondQuad, kQuadIndices, colorState());
  expect(renderer.flush(), "a later UI batch recovers after one failed upload");
  renderer.end();

  expect(backend.submissions.size() == 1 &&
             backend.submissions.front().vertices.front().x == 20.0F,
         "a failed UI batch does not corrupt later painter-order geometry");
}

void testFailedStateTransitionReportsTheRejectedAppend() {
  RecordingBackend backend;
  backend.rejectedSubmissions = 1;
  rendering::UiBatchRenderer renderer(backend);
  auto secondState = colorState();
  secondState.program = bgfx::ProgramHandle{8};

  renderer.begin();
  expect(renderer.appendColor(kFirstQuad, kQuadIndices, colorState()),
         "the first queued UI quad is accepted");
  expect(!renderer.appendColor(kSecondQuad, kQuadIndices, secondState),
         "a rejected state-transition upload reports the append failure");
  renderer.end();

  expect(backend.submissions.size() == 1 &&
             backend.submissions.front().vertices.front().x == 20.0F,
         "the batch after a rejected transition remains independently drawable");
}

} // namespace

int main() {
  testCompatibleColoredGeometryCoalesces();
  testRenderContextScopeFlushesOnlyAtExplicitBoundary();
  testDirectViewFlushesDecorationsBeforeAndUiAfterCustomDraw();
  testProgramBoundaryPreservesSubmissionOrder();
  testStateBoundariesPreserveSubmissionOrder();
  testBatchSplitsBeforeUint16IndexLimit();
  testFailedBatchDoesNotBlockLaterSubmission();
  testFailedStateTransitionReportsTheRejectedAppend();
  if (failures != 0) {
    return 1;
  }
  std::cout << "UI batch renderer tests passed\n";
  return 0;
}
