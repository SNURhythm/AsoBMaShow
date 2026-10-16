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
  void beginFrame() noexcept override { ++begunFrames; }
  std::size_t begunFrames = 0;
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
    order.push_back('D');
    return true;
  }

  bool submitRetained(const std::shared_ptr<const rendering::UiRetainedGeometry> &geometry,
                      const rendering::UiBatchState &state) noexcept override {
    if (rejectRetained) return false;
    retained.push_back(geometry);
    retainedStates.push_back(state);
    order.push_back('R');
    return true;
  }

  std::vector<CapturedSubmission> submissions;
  std::vector<std::shared_ptr<const rendering::UiRetainedGeometry>> retained;
  std::vector<rendering::UiBatchState> retainedStates;
  std::vector<char> order;
  std::size_t rejectedSubmissions = 0;
  bool rejectRetained = false;
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
  const auto beforeFailure = ui::revisions();
  expect(!renderer.flush(), "a rejected dynamic upload reports failure");
  expect(ui::revisions() != beforeFailure,
         "a failed upload requests another presentation attempt");
  renderer.appendColor(kSecondQuad, kQuadIndices, colorState());
  expect(renderer.flush(), "a later UI batch recovers after one failed upload");
  renderer.end();

  expect(backend.submissions.size() == 1 &&
             backend.submissions.front().vertices.front().x == 20.0F,
         "a failed UI batch does not corrupt later painter-order geometry");
}

void testRetainedGeometryPreservesOrderAndIdentity() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);
  auto geometry = std::make_shared<rendering::UiRetainedGeometry>();
  geometry->layout.begin(bgfx::RendererType::Noop)
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true).end();
  const auto bytes = std::as_bytes(std::span{kFirstQuad});
  geometry->vertices.assign(bytes.begin(), bytes.end());
  geometry->indices.assign(kQuadIndices.begin(), kQuadIndices.end());
  auto state = colorState();
  state.scissor = {2, 3, 100, 200};
  state.uniformCount = 6;
  for (std::size_t i = 0; i < state.uniformCount; ++i)
    state.uniforms[i] = {bgfx::UniformHandle{static_cast<std::uint16_t>(i)}, {float(i), 0, 0, 0}};

  renderer.begin();
  renderer.appendColor(kFirstQuad, kQuadIndices, colorState());
  expect(renderer.submitRetained(geometry, state), "retained geometry submits inside a UI batch");
  renderer.appendColor(kSecondQuad, kQuadIndices, colorState());
  renderer.end();
  expect(backend.order == std::vector<char>({'D', 'R', 'D'}),
         "retained geometry draws between earlier and later UI content");
  renderer.begin();
  state.uniforms[5].value[0] = 99;
  renderer.submitRetained(geometry, state);
  renderer.end();
  expect(backend.retained.size() == 2 && backend.retained[0] == backend.retained[1] &&
             backend.retained[1].get() == geometry.get(),
         "animation reuses the same immutable geometry without staging a new mesh");
  expect(backend.retainedStates[0].scissor->x == 2 &&
             backend.retainedStates[0].uniforms[5].value[0] == 5 &&
             backend.retainedStates[1].uniforms[5].value[0] == 99,
         "retained draws preserve clipping and refresh all shader parameters");
  expect(!renderer.submitRetained(geometry, state),
         "retained geometry cannot draw outside an active UI batch");
}

void testEveryRequestedFrameSubmitsImmediately() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);
  for (int frame = 0; frame < 3; ++frame) {
    renderer.beginFrame();
    renderer.begin();
    renderer.appendColor(kFirstQuad, kQuadIndices, colorState());
    renderer.end();
    expect(backend.begunFrames == frame + 1 && backend.submissions.size() == frame + 1,
           "each requested frame resets resources and immediately submits its UI");
  }
}

void testRendererInvalidationWakesThePresentationSchedule() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);
  ui::PresentationSchedule schedule;
  schedule.didPresent(ui::revisions(), {}, 100);
  expect(!schedule.shouldRender(101), "fixture starts with a settled presentation");
  renderer.invalidateFrame();
  expect(schedule.shouldRender(101), "surface invalidation wakes a settled presentation");
  schedule.didPresent(ui::revisions(), {}, 101);
  rendering::UiBatchRenderer::notifyTextureChanged();
  expect(schedule.shouldRender(102), "texture pixel or handle changes wake presentation");
}

void testRetainedFailureUsesImmediateFallbackInOrder() {
  RecordingBackend backend;
  backend.rejectRetained = true;
  rendering::UiBatchRenderer renderer(backend);
  auto geometry = std::make_shared<rendering::UiRetainedGeometry>();
  renderer.beginFrame();
  renderer.begin();
  renderer.appendColor(kFirstQuad, kQuadIndices, colorState());
  const bool submitted = renderer.submitRetained(geometry, colorState());
  expect(!submitted, "retained effects return the actual backend failure");
  if (!submitted) renderer.appendColor(kFirstQuad, kQuadIndices, colorState());
  renderer.appendColor(kSecondQuad, kQuadIndices, colorState());
  renderer.end();
  expect(backend.order == std::vector<char>({'D', 'D'}),
         "failed retained effects submit their CPU fallback after the prefix");
  expect(backend.submissions.back().vertices.size() == 8,
         "CPU fallback and subsequent content both reach the backend");
}

} // namespace

int main() {
  testEveryRequestedFrameSubmitsImmediately();
  testRendererInvalidationWakesThePresentationSchedule();
  testRetainedFailureUsesImmediateFallbackInOrder();
  testCompatibleColoredGeometryCoalesces();
  testRenderContextScopeFlushesOnlyAtExplicitBoundary();
  testDirectViewFlushesDecorationsBeforeAndUiAfterCustomDraw();
  testProgramBoundaryPreservesSubmissionOrder();
  testStateBoundariesPreserveSubmissionOrder();
  testBatchSplitsBeforeUint16IndexLimit();
  testFailedBatchDoesNotBlockLaterSubmission();
  testRetainedGeometryPreservesOrderAndIdentity();
  if (failures != 0) {
    return 1;
  }
  std::cout << "UI batch renderer tests passed\n";
  return 0;
}
