#include "audio/GameplayBgaFrame.h"
#include "rendering/RenderPlan.h"
#include "rendering/SkinQuadBatchRenderer.h"
#include "rendering/common.h"
#include "skin/beatoraja/Skin2DRenderer.h"
#include "view/View.h"

// This target deliberately compiles the narrow production adapter directly:
// Task 19 keeps build-system edits out of scope while exercising the actual
// two-phase integration rather than a test copy of its orchestration.
#include "skin/beatoraja/Skin2DRendererSubmit.cpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
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
} // namespace rendering

namespace {

int failures = 0;

void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool near(float left, float right) { return std::abs(left - right) <= 0.0001F; }

struct FakeResources final : skin::SkinPreparedResourceView {
  const skin::PreparedSkinResource *
  find(skin::SkinResourceId id) const noexcept override {
    const auto found = images.find(id);
    return found == images.end() ? nullptr : &found->second;
  }

  const skin::SkinResolvedRegion *
  findResolvedRegion(skin::SkinResourceId,
                     const skin::SkinSourceRect &) const noexcept override {
    return nullptr;
  }

  const skin::PreparedSkinTextAtlas *
  findTextAtlas(skin::SkinTextAtlasId id) const noexcept override {
    const auto found = atlases.find(id);
    return found == atlases.end() ? nullptr : &found->second;
  }

  const skin::PreparedSkinTextAtlas *
  findTextAtlasForObject(skin::SkinObjectId) const noexcept override {
    return nullptr;
  }

  std::map<skin::SkinResourceId, skin::PreparedSkinResource> images;
  std::map<skin::SkinTextAtlasId, skin::PreparedSkinTextAtlas> atlases;
};

struct CapturedBatch {
  std::vector<rendering::SkinQuadGpuVertex> vertices;
  std::vector<std::uint16_t> indices;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  rendering::SkinBatchTopology topology =
      rendering::SkinBatchTopology::Triangles;
  skin::SkinBlendMode blend = skin::SkinBlendMode::Normal;
  skin::SkinFilterMode filter = skin::SkinFilterMode::Nearest;
  std::optional<skin::UiLogicalRect> scissor;
  std::uint64_t bgfxState = 0;
  std::uint32_t samplerFlags = 0;
  bool textured = false;
};

struct FakeBackend final : rendering::SkinQuadBatchBackend {
  bool preflightVertexLayouts(
      std::span<const bgfx::VertexLayout *const> layouts) override {
    ++layoutPreflightCalls;
    preflightLayoutCount = layouts.size();
    layoutsReady = layoutPreflightResult;
    return layoutPreflightResult;
  }

  bool preflightSamplers(
      std::span<const skin::SkinFilterMode> filters) override {
    ++samplerPreflightCalls;
    for (const auto filter : filters) {
      if (unavailableSampler && filter == *unavailableSampler) {
        return false;
      }
    }
    return true;
  }

  bool reserve(
      std::size_t vertexCount, std::size_t indexCount,
      std::size_t skinAllocationCount,
      const GameplayBgaTransientRequirements &bgaRequirements) override {
    ++reserveCalls;
    layoutsReadyAtReserve = layoutsReady;
    reservedVertices = vertexCount;
    reservedIndices = indexCount;
    reservedSkinAllocations = skinAllocationCount;
    reservedBgaRequirements = bgaRequirements;
    if (throwOnReserve) {
      throw std::bad_alloc();
    }
    return reserveResult;
  }

  void submit(const rendering::SkinQuadBackendBatch &batch) override {
    CapturedBatch captured;
    captured.vertices.assign(batch.vertices.begin(), batch.vertices.end());
    captured.indices.assign(batch.indices.begin(), batch.indices.end());
    captured.texture = batch.texture;
    captured.topology = batch.topology;
    captured.blend = batch.blend;
    captured.filter = batch.filter;
    captured.scissor = batch.scissor;
    captured.bgfxState = batch.bgfxState;
    captured.samplerFlags = batch.samplerFlags;
    captured.textured = batch.textured;
    batches.push_back(std::move(captured));
    if (submissionOrder != nullptr) {
      submissionOrder->push_back("quad:" + std::to_string(batch.texture.idx));
    }
    if (afterSubmit) {
      afterSubmit();
    }
  }

  bool reserveResult = true;
  bool layoutPreflightResult = true;
  bool throwOnReserve = false;
  int reserveCalls = 0;
  int samplerPreflightCalls = 0;
  int layoutPreflightCalls = 0;
  std::size_t preflightLayoutCount = 0;
  bool layoutsReady = false;
  bool layoutsReadyAtReserve = false;
  std::size_t reservedVertices = 0;
  std::size_t reservedIndices = 0;
  std::size_t reservedSkinAllocations = 0;
  GameplayBgaTransientRequirements reservedBgaRequirements;
  std::optional<skin::SkinFilterMode> unavailableSampler;
  std::vector<CapturedBatch> batches;
  std::vector<std::string> *submissionOrder = nullptr;
  std::function<void()> afterSubmit;
};

struct LayoutPoolControl {
  int createCalls = 0;
  int failOnCreate = -1;
  std::array<std::uint16_t, 4> destroyed{};
  std::size_t destroyedCount = 0;
};

bgfx::VertexLayoutHandle fakeCreateVertexLayout(
    const bgfx::VertexLayout &, void *context) noexcept {
  auto &control = *static_cast<LayoutPoolControl *>(context);
  ++control.createCalls;
  if (control.createCalls == control.failOnCreate) {
    return BGFX_INVALID_HANDLE;
  }
  return bgfx::VertexLayoutHandle{
      static_cast<std::uint16_t>(900 + control.createCalls)};
}

void fakeDestroyVertexLayout(bgfx::VertexLayoutHandle handle,
                             void *context) noexcept {
  auto &control = *static_cast<LayoutPoolControl *>(context);
  control.destroyed[control.destroyedCount++] = handle.idx;
}

struct FakeBgaSubmitter final : IGameplayBgaSubmitter {
  PreparedGameplayBgaFrame
  prepareVisualFrameAt(std::uint64_t, std::int64_t,
                       const GameplayBgaMissState &) override {
    return {};
  }

  BgaPreflightResult
  preflight(const PreparedGameplayBgaFrame &frame,
            std::span<const BgaDrawTarget> targets) override {
    ++preflightCalls;
    preflightFrame = frame;
    preflightTargets.assign(targets.begin(), targets.end());
    return {.ready = preflightReady,
            .requirements = preflightReady ? preflightRequirements
                                           : GameplayBgaTransientRequirements{}};
  }

  void commitPrepared(const PreparedGameplayBgaFrame &) noexcept override {
    ++commitCalls;
  }

  void submitPrepared(const PreparedGameplayBgaFrame &,
                      const BgaDrawTarget &target) noexcept override {
    submittedTargets.push_back(target);
    if (submissionOrder != nullptr) {
      const char role = target.role == GameplayBgaRole::Base    ? 'B'
                        : target.role == GameplayBgaRole::Layer ? 'L'
                                                               : 'M';
      submissionOrder->push_back("bga:" + std::string(1, role) + ":" +
                                 std::to_string(target.authoredOrdinal));
    }
  }

  void finalizePrepared(const PreparedGameplayBgaFrame &) noexcept override {
    ++finalizeCalls;
  }

  void submitFullscreen(const PreparedGameplayBgaFrame &) noexcept override {}

  bool preflightReady = true;
  GameplayBgaTransientRequirements preflightRequirements;
  int preflightCalls = 0;
  int commitCalls = 0;
  int finalizeCalls = 0;
  PreparedGameplayBgaFrame preflightFrame;
  std::vector<BgaDrawTarget> preflightTargets;
  std::vector<BgaDrawTarget> submittedTargets;
  std::vector<std::string> *submissionOrder = nullptr;
};

skin::SkinTexturedQuadCommand
quad(skin::SkinResourceId resource, skin::SkinBlendMode blend,
     skin::SkinFilterMode filter,
     std::optional<skin::UiLogicalRect> scissor = std::nullopt,
     float x = 0.0F) {
  skin::SkinTexturedQuadCommand result;
  result.resource = resource;
  result.state = {.blend = blend, .filter = filter, .scissor = scissor};
  result.vertices = {
      skin::SkinVertex{
          .x = x + 1.0F, .y = 8.0F, .u = 0.1F, .v = 0.9F, .rgba = 0x80402010U},
      skin::SkinVertex{
          .x = x + 9.0F, .y = 7.0F, .u = 0.8F, .v = 0.8F, .rgba = 0x80604020U},
      skin::SkinVertex{
          .x = x + 8.0F, .y = 1.0F, .u = 0.7F, .v = 0.2F, .rgba = 0x80806030U},
      skin::SkinVertex{
          .x = x, .y = 2.0F, .u = 0.2F, .v = 0.3F, .rgba = 0x80a08040U},
  };
  return result;
}

skin::SkinDrawCommand command(std::uint32_t ordinal,
                              skin::SkinDrawPayload payload) {
  return {.authoredOrdinal = ordinal,
          .sourceObject = ordinal,
          .payload = std::move(payload)};
}

skin::SkinBgaCommand bga(std::uint32_t ordinal, double x = 10.0) {
  skin::SkinBgaCommand result;
  result.authoredOrdinal = ordinal;
  result.authoredGeometry = {
      .rect = {.x = x, .y = 20.0, .width = 30.0, .height = 40.0},
      .clip = skin::AuthoredRect{.x = x + 2.0,
                                 .y = 23.0,
                                 .width = 7.0,
                                 .height = 11.0},
      .centerX = 0.5,
      .centerY = 0.5,
      .rgba = {0.1F, 0.2F, 0.3F, 0.4F},
      .blend = skin::SkinBlendMode::Additive,
      .stretch = skin::SkinStretchMode::KeepAspectRatioFitInner};
  result.viewport = {
      .authoredToUi = {.m00 = 2.0,
                       .m01 = 0.5,
                       .tx = 5.0,
                       .m10 = 0.25,
                       .m11 = -3.0,
                       .ty = 100.0},
      .safeUiBounds = {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
      .valid = true};
  return result;
}

PreparedGameplayBgaSurface surface(GameplayBgaRole role,
                                   std::uint64_t token) {
  return {.role = role,
          .mediaKind = GameplayBgaMediaKind::Image,
          .surfaceToken = token,
          .sourceWidth = 640,
          .sourceHeight = 480};
}

FakeResources resources() {
  FakeResources result;
  result.images.emplace(
      11, skin::PreparedSkinResource{
              .id = 11, .texture = {.idx = 101}, .width = 64, .height = 32});
  result.images.emplace(
      12, skin::PreparedSkinResource{
              .id = 12, .texture = {.idx = 102}, .width = 32, .height = 32});
  skin::PreparedSkinTextAtlas atlas{
      .id = 21,
      .key = {.font = 77,
              .pointSize = 24,
              .fallbackChainDigest = "77:0|fixture.ttf:0"},
      .texture = {.idx = 201},
      .width = 128,
      .height = 64,
      .glyphs = {{U'A', {.region = {.x = 0, .y = 0, .w = 8, .h = 12}}}},
      .ascent = 10,
      .capHeight = 8,
      .descent = -2,
      .lineHeight = 12};
  result.atlases.emplace(atlas.id, std::move(atlas));
  return result;
}

void testWholeBufferPreflightUsesExactCatalogIds() {
  auto prepared = resources();
  RenderContext context;
  FakeBackend backend;
  rendering::SkinQuadBatchRenderer renderer(backend);

  std::vector<skin::SkinDrawCommand> commands;
  commands.push_back(command(
      1, quad(11, skin::SkinBlendMode::Normal, skin::SkinFilterMode::Nearest)));
  commands.push_back(command(2, quad(999, skin::SkinBlendMode::Normal,
                                     skin::SkinFilterMode::Nearest)));
  renderer.begin(context, prepared);
  expect(!renderer.submit(commands),
         "a stale image ID rejects the whole command span");
  renderer.flush();
  expect(backend.reserveCalls == 0 && backend.batches.empty(),
         "catalog preflight fails before reserving or submitting backend data");

  skin::SkinGlyphRunCommand glyphs;
  glyphs.atlas = 21;
  glyphs.state = {.blend = skin::SkinBlendMode::Normal,
                  .filter = skin::SkinFilterMode::Linear};
  glyphs.glyphs.push_back({.codepoint = U'A',
                           .vertices = quad(11, skin::SkinBlendMode::Normal,
                                            skin::SkinFilterMode::Nearest)
                                           .vertices});
  commands = {command(3, glyphs)};
  renderer.begin(context, prepared);
  expect(renderer.submit(commands),
         "an exact composite-key atlas ID with prepared glyph metrics passes");
  renderer.flush();
  expect(backend.batches.size() == 1 &&
             backend.batches.front().texture.idx == 201 &&
             backend.batches.front().textured,
         "glyph submission resolves only the explicitly selected atlas handle");

  backend.batches.clear();
  std::get<skin::SkinGlyphRunCommand>(commands.front().payload)
      .glyphs.front()
      .codepoint = U'B';
  renderer.begin(context, prepared);
  expect(!renderer.submit(commands),
         "a glyph absent from the exact prepared atlas is stale");
  renderer.flush();
  expect(backend.batches.empty(),
         "stale glyph metrics cannot cause a partial text submission");

  std::get<skin::SkinGlyphRunCommand>(commands.front().payload).atlas = 22;
  std::get<skin::SkinGlyphRunCommand>(commands.front().payload)
      .glyphs.front()
      .codepoint = U'A';
  renderer.begin(context, prepared);
  expect(
      !renderer.submit(commands),
      "renderer never substitutes a different point-size atlas sharing a font");
}

void testVerticesStatesScissorsAndSequentialFlushes() {
  auto prepared = resources();
  RenderContext context;
  FakeBackend backend;
  rendering::SkinQuadBatchRenderer renderer(backend);
  const skin::UiLogicalRect clip{
      .x = 3.0, .y = 4.0, .width = 20.0, .height = 10.0};

  std::vector<skin::SkinDrawCommand> commands = {
      command(1, quad(11, skin::SkinBlendMode::Normal,
                      skin::SkinFilterMode::Nearest)),
      command(2, quad(11, skin::SkinBlendMode::Normal,
                      skin::SkinFilterMode::Nearest, std::nullopt, 20.0F)),
      command(3, quad(12, skin::SkinBlendMode::Additive,
                      skin::SkinFilterMode::Nearest)),
      command(4, quad(12, skin::SkinBlendMode::Subtractive,
                      skin::SkinFilterMode::Nearest, clip)),
      command(5, quad(12, skin::SkinBlendMode::Multiply,
                      skin::SkinFilterMode::Linear, clip)),
      command(6, quad(12, skin::SkinBlendMode::Inverse,
                      skin::SkinFilterMode::Linear, clip)),
  };

  renderer.begin(context, prepared);
  expect(renderer.submit(commands), "valid state sequence passes preflight");
  renderer.flush();
  expect(backend.reserveCalls == 1 && backend.reservedVertices == 24 &&
             backend.reservedIndices == 36 &&
             backend.reservedSkinAllocations == 5,
         "whole-frame transient capacity is reserved before the first draw");
  expect(backend.batches.size() == 5,
         "only adjacent commands with identical resource and state batch");
  expect(backend.batches[0].vertices.size() == 8 &&
             backend.batches[0].indices.size() == 12 &&
             backend.batches[0].texture.idx == 101,
         "the first two authored quads remain one sequential batch");
  expect(
      near(backend.batches[0].vertices[0].x, 1.0F) &&
          near(backend.batches[0].vertices[1].y, 7.0F) &&
          near(backend.batches[0].vertices[2].u, 0.7F) &&
          backend.batches[0].vertices[3].abgr == 0x80a08040U,
      "rotated per-vertex position, UV, and ABGR values pass through exactly");
  expect(backend.batches[1].blend == skin::SkinBlendMode::Additive &&
             backend.batches[2].blend == skin::SkinBlendMode::Subtractive &&
             backend.batches[3].blend == skin::SkinBlendMode::Multiply &&
             backend.batches[4].blend == skin::SkinBlendMode::Inverse,
         "blend changes flush in authored order");
  expect(backend.batches[2].scissor &&
             backend.batches[2].scissor->x == clip.x &&
             backend.batches[3].filter == skin::SkinFilterMode::Linear,
         "logical scissor and sampler changes are explicit batch state");
  expect(backend.batches[0].bgfxState ==
                 rendering::skinBgfxState(skin::SkinBlendMode::Normal) &&
             backend.batches[1].bgfxState ==
                 rendering::skinBgfxState(skin::SkinBlendMode::Additive) &&
             backend.batches[2].bgfxState ==
                 rendering::skinBgfxState(skin::SkinBlendMode::Subtractive) &&
             backend.batches[3].bgfxState ==
                 rendering::skinBgfxState(skin::SkinBlendMode::Multiply) &&
             backend.batches[4].bgfxState ==
                 rendering::skinBgfxState(skin::SkinBlendMode::Inverse),
         "normal/add/subtract/multiply/inverse map to exact backend states");
  expect(backend.batches[0].samplerFlags ==
                 rendering::skinSamplerFlags(skin::SkinFilterMode::Nearest) &&
             backend.batches[3].samplerFlags ==
                 rendering::skinSamplerFlags(skin::SkinFilterMode::Linear),
         "nearest and linear filtering map to explicit sampler flags");
}

void testExactBackendStateMapping() {
  constexpr std::uint64_t write = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
  expect(rendering::skinBgfxState(skin::SkinBlendMode::Normal) ==
             (write | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                            BGFX_STATE_BLEND_INV_SRC_ALPHA)),
         "normal blend is SRC_ALPHA / INV_SRC_ALPHA");
  expect(rendering::skinBgfxState(skin::SkinBlendMode::Additive) ==
             (write | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                            BGFX_STATE_BLEND_ONE)),
         "additive blend is SRC_ALPHA / ONE, not ONE / ONE");
  expect(rendering::skinBgfxState(skin::SkinBlendMode::Subtractive) ==
             (write |
              BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                    BGFX_STATE_BLEND_ONE) |
              BGFX_STATE_BLEND_EQUATION(BGFX_STATE_BLEND_EQUATION_SUB)),
         "subtractive blend uses the intended source-minus-destination state");
  expect(rendering::skinBgfxState(skin::SkinBlendMode::Multiply) ==
             (write | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ZERO,
                                            BGFX_STATE_BLEND_SRC_COLOR)),
         "multiply blend is ZERO / SRC_COLOR");
  expect(rendering::skinBgfxState(skin::SkinBlendMode::Inverse) ==
             (write | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_INV_DST_COLOR,
                                            BGFX_STATE_BLEND_ZERO)),
         "inverse blend is INV_DST_COLOR / ZERO");
  expect(rendering::skinSamplerFlags(skin::SkinFilterMode::Nearest) ==
                 (BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_POINT) &&
             rendering::skinSamplerFlags(skin::SkinFilterMode::Linear) ==
                 BGFX_SAMPLER_UVW_CLAMP,
         "nearest and linear samplers both clamp and differ only by POINT");
}

void testPrimitiveTopologyAndBgaRequiresFallbackUntilIntegrated() {
  auto prepared = resources();
  RenderContext context;
  FakeBackend backend;
  rendering::SkinQuadBatchRenderer renderer(backend);

  skin::SkinPrimitiveCommand solid{
      .kind = skin::SkinPrimitiveKind::SolidQuad,
      .vertices = {{.x = 0, .y = 0, .rgba = 0xff0000ffU},
                   {.x = 10, .y = 0, .rgba = 0xff00ff00U},
                   {.x = 10, .y = 10, .rgba = 0xffff0000U},
                   {.x = 0, .y = 10, .rgba = 0xffffffffU}}};
  skin::SkinPrimitiveCommand line{
      .kind = skin::SkinPrimitiveKind::LineStrip,
      .vertices = {{.x = 1, .y = 1}, {.x = 2, .y = 3}, {.x = 5, .y = 8}}};
  std::vector<skin::SkinDrawCommand> commands = {command(1, solid),
                                                 command(2, line)};

  renderer.begin(context, prepared);
  expect(renderer.submit(commands), "prepared primitives are submit-ready");
  renderer.flush();
  expect(backend.batches.size() == 2 && !backend.batches[0].textured &&
             backend.batches[0].topology ==
                 rendering::SkinBatchTopology::Triangles &&
             backend.batches[1].topology ==
                 rendering::SkinBatchTopology::LineStrip,
         "solid and line primitives use explicit topology");
  expect(backend.batches[0].indices ==
                 std::vector<std::uint16_t>{0, 1, 2, 0, 2, 3} &&
             backend.batches[1].indices == std::vector<std::uint16_t>{0, 1, 2},
         "primitive index order is deterministic");

  backend.batches.clear();
  backend.reserveCalls = 0;
  skin::SkinBgaCommand bga;
  commands = {command(3, solid), command(4, bga), command(5, line)};
  renderer.begin(context, prepared);
  expect(!renderer.submit(commands), "BGA commands require built-in fallback "
                                     "until the BGA submit bridge exists");
  renderer.flush();
  expect(
      backend.reserveCalls == 0 && backend.batches.empty(),
      "an unsupported BGA rejects the whole span before primitive submission");
}

void testTwoPhaseSkinSubmissionPreservesMixedAuthoredOrder() {
  auto prepared = resources();
  RenderContext context;
  std::vector<std::string> submissionOrder;
  FakeBackend backend;
  backend.submissionOrder = &submissionOrder;
  rendering::SkinQuadBatchRenderer quadRenderer(backend);
  skin::Skin2DRenderer skinRenderer;
  FakeBgaSubmitter bgaSubmitter;
  bgaSubmitter.preflightRequirements = {
      .vertexBytes = 384, .vertexAlignmentPadding = 36, .indexCount = 24};
  bgaSubmitter.submissionOrder = &submissionOrder;
  const PreparedGameplayBgaFrame frame{
      .sequence = 7,
      .composition = GameplayBgaComposition::BaseThenLayer,
      .base = surface(GameplayBgaRole::Base, 70),
      .layer = surface(GameplayBgaRole::Layer, 71)};
  const skin::SkinCommandBuffer buffer{
      .frameSerial = 11,
      .commands = {
          command(10, quad(11, skin::SkinBlendMode::Normal,
                           skin::SkinFilterMode::Nearest)),
          command(20, bga(20)),
          command(30, quad(12, skin::SkinBlendMode::Normal,
                           skin::SkinFilterMode::Nearest)),
          command(40, bga(40, 50.0)),
          command(50, quad(11, skin::SkinBlendMode::Normal,
                           skin::SkinFilterMode::Nearest)),
      }};

  expect(skinRenderer.submit(buffer, prepared, context, quadRenderer, frame,
                             bgaSubmitter),
         "two-phase skin submission accepts mixed quad/BGA authored slots");
  expect(submissionOrder ==
             std::vector<std::string>{"quad:101", "bga:B:20", "bga:L:20",
                                      "quad:102", "bga:B:40", "bga:L:40",
                                      "quad:101"},
         "quad segments and every stable BGA role remain contiguous in exact "
         "authored order");
  expect(backend.samplerPreflightCalls == 1 && backend.reserveCalls == 1 &&
             backend.layoutPreflightCalls == 1 &&
             backend.preflightLayoutCount == 1 &&
             backend.layoutsReadyAtReserve &&
             backend.reservedVertices == 12 && backend.reservedIndices == 18 &&
             backend.reservedSkinAllocations == 3 &&
             bgaSubmitter.preflightCalls == 1 &&
             bgaSubmitter.preflightTargets.size() == 4 &&
             bgaSubmitter.submittedTargets.size() == 4 &&
             bgaSubmitter.commitCalls == 1 &&
             bgaSubmitter.finalizeCalls == 1 &&
             backend.reservedBgaRequirements.vertexBytes == 384 &&
             backend.reservedBgaRequirements.vertexAlignmentPadding == 36 &&
             backend.reservedBgaRequirements.indexCount == 24,
         "all quad segments and all BGA targets preflight exactly once before "
         "one combined capacity decision and no-fail submission");
  if (bgaSubmitter.preflightTargets.size() != 4) {
    return;
  }
  const auto &base = bgaSubmitter.preflightTargets[0];
  const auto &layer = bgaSubmitter.preflightTargets[1];
  expect(base.role == GameplayBgaRole::Base &&
             layer.role == GameplayBgaRole::Layer &&
             base.viewId == rendering::ui_view &&
             base.authoredOrdinal == 20 && layer.authoredOrdinal == 20 &&
             base.destination[0].x == 35.0F &&
             base.destination[0].y == 42.5F &&
             base.destination[1].x == 95.0F &&
             base.destination[1].y == 50.0F &&
             base.destination[2].x == 115.0F &&
             base.destination[2].y == -70.0F &&
             base.destination[3].x == 55.0F &&
             base.destination[3].y == -77.5F &&
             base.stretch == skin::SkinStretchMode::KeepAspectRatioFitInner &&
             base.authoredProjection &&
             base.authoredProjection->x == 10.0 &&
             base.authoredProjection->y == 20.0 &&
             base.authoredProjection->width == 30.0 &&
             base.authoredProjection->height == 40.0 &&
             base.authoredProjection->centerX == 0.5 &&
             base.authoredProjection->centerY == 0.5 &&
             base.authoredProjection->angleDegrees == 0.0 &&
             base.authoredProjection->authoredToUi.m00 == 2.0 &&
             base.authoredProjection->authoredToUi.m01 == 0.5 &&
             base.authoredProjection->authoredToUi.tx == 5.0 &&
             base.authoredProjection->authoredToUi.m10 == 0.25 &&
             base.authoredProjection->authoredToUi.m11 == -3.0 &&
             base.authoredProjection->authoredToUi.ty == 100.0 &&
             base.tint == std::array<float, 4>{0.1F, 0.2F, 0.3F, 0.4F} &&
             base.blend == skin::SkinBlendMode::Additive && base.clip &&
             base.clip->x == 40.5 && base.clip->y == 1.0 &&
             base.clip->width == 19.5 && base.clip->height == 34.75,
         "role materialization preserves authored geometry until source-aware "
         "stretch while retaining projected placeholder geometry and draw state");
}

void testCommittedSubmissionCannotReturnFallbackSignal() {
  auto prepared = resources();
  RenderContext context;
  FakeBackend backend;
  rendering::SkinQuadBatchRenderer quadRenderer(backend);
  skin::Skin2DRenderer skinRenderer;
  FakeBgaSubmitter submitter;
  const PreparedGameplayBgaFrame frame{
      .sequence = 81,
      .composition = GameplayBgaComposition::BaseThenLayer,
      .base = surface(GameplayBgaRole::Base, 810)};
  const skin::SkinCommandBuffer buffer{
      .frameSerial = 82,
      .commands = {
          command(1, quad(11, skin::SkinBlendMode::Normal,
                          skin::SkinFilterMode::Nearest)),
          command(2, bga(2)),
          command(3, quad(12, skin::SkinBlendMode::Normal,
                          skin::SkinFilterMode::Nearest)),
      }};
  bool invalidatedAfterFirstDraw = false;
  backend.afterSubmit = [&] {
    if (!invalidatedAfterFirstDraw) {
      invalidatedAfterFirstDraw = true;
      quadRenderer.begin(context, prepared);
    }
  };

  const bool submitted = skinRenderer.submit(
      buffer, prepared, context, quadRenderer, frame, submitter);

  expect(submitted && invalidatedAfterFirstDraw && submitter.commitCalls == 1 &&
             submitter.finalizeCalls == 1 &&
             submitter.submittedTargets.size() == 1,
         "once BGA capacity is committed, submission always finalizes and can "
         "never signal built-in fallback after a draw");
}

void testBgaCompositionExpandsPlaceholdersAndMissingRolesExactly() {
  const auto run = [](const PreparedGameplayBgaFrame &frame,
                      std::span<const GameplayBgaRole> expectedRoles,
                      std::string_view message) {
    auto prepared = resources();
    RenderContext context;
    FakeBackend backend;
    rendering::SkinQuadBatchRenderer quadRenderer(backend);
    skin::Skin2DRenderer skinRenderer;
    FakeBgaSubmitter submitter;
    const skin::SkinCommandBuffer buffer{
        .frameSerial = 3, .commands = {command(77, bga(77))}};
    const bool submitted = skinRenderer.submit(
        buffer, prepared, context, quadRenderer, frame, submitter);
    bool rolesMatch = submitter.preflightTargets.size() == expectedRoles.size();
    for (std::size_t index = 0;
         rolesMatch && index < expectedRoles.size(); ++index) {
      rolesMatch =
          submitter.preflightTargets[index].role == expectedRoles[index] &&
          submitter.preflightTargets[index].authoredOrdinal == 77;
    }
    if (rolesMatch && !expectedRoles.empty()) {
      const auto &target = submitter.preflightTargets.front();
      rolesMatch = target.destination[0].x == 35.0F &&
                   target.destination[0].y == 42.5F &&
                   target.destination[2].x == 115.0F &&
                   target.destination[2].y == -70.0F &&
                   target.stretch ==
                       skin::SkinStretchMode::KeepAspectRatioFitInner;
    }
    expect(submitted && submitter.preflightCalls == 1 && rolesMatch &&
               submitter.submittedTargets.size() == expectedRoles.size() &&
               backend.batches.empty(),
           message);
  };

  const std::array blankRoles{GameplayBgaRole::Base};
  run({.sequence = 1, .composition = GameplayBgaComposition::Blank},
      blankRoles,
      "Blank expands to one full-destination base placeholder");

  const std::array baseLayerRoles{GameplayBgaRole::Base,
                                  GameplayBgaRole::Layer};
  run({.sequence = 2,
       .composition = GameplayBgaComposition::BaseThenLayer,
       .layer = surface(GameplayBgaRole::Layer, 22)},
      baseLayerRoles,
      "missing base expands to a base placeholder before its available layer");

  run({.sequence = 3, .composition = GameplayBgaComposition::MissOnly}, {},
      "missing miss suppresses base/layer and emits no BGA target");

  const std::array missRoles{GameplayBgaRole::Miss};
  run({.sequence = 4,
       .composition = GameplayBgaComposition::MissOnly,
       .miss = surface(GameplayBgaRole::Miss, 44)},
      missRoles, "available miss expands to only its miss target");
}

void testUnclippedBgaUsesProjectedSkinResolutionScissor() {
  auto command = bga(1);
  command.authoredGeometry.clip.reset();
  command.viewport.safeUiBounds =
      {.x = 0.0, .y = 0.0, .width = 120.0, .height = 90.0};
  command.viewport.projectedUiBounds =
      skin::UiLogicalRect{.x = 0.0, .y = 11.25, .width = 120.0, .height = 67.5};

  const auto target = skin::projectBgaTarget(command, GameplayBgaRole::Base);
  expect(target && target->clip && target->clip->x == 0.0 &&
             target->clip->y == 11.25 && target->clip->width == 120.0 &&
             target->clip->height == 67.5,
         "unclipped BGA targets are scissored to the fitted skin resolution");
}

void testTwoPhaseSubmissionFailuresAreZeroDrawAtomic() {
  const PreparedGameplayBgaFrame frame{
      .sequence = 8,
      .composition = GameplayBgaComposition::BaseThenLayer,
      .base = surface(GameplayBgaRole::Base, 80)};

  {
    auto prepared = resources();
    RenderContext context;
    FakeBackend backend;
    rendering::SkinQuadBatchRenderer quadRenderer(backend);
    skin::Skin2DRenderer skinRenderer;
    FakeBgaSubmitter submitter;
    submitter.preflightReady = false;
    const skin::SkinCommandBuffer buffer{
        .frameSerial = 9,
        .commands = {
            command(1, quad(11, skin::SkinBlendMode::Normal,
                            skin::SkinFilterMode::Nearest)),
            command(2, bga(2)),
            command(3, quad(12, skin::SkinBlendMode::Normal,
                            skin::SkinFilterMode::Nearest)),
        }};
    expect(skinRenderer.submit(buffer, prepared, context, quadRenderer, frame,
                               submitter) &&
               backend.reserveCalls == 1 && backend.batches.size() == 2 &&
               submitter.preflightCalls == 1 &&
               submitter.commitCalls == 0 &&
               submitter.finalizeCalls == 1 &&
               submitter.submittedTargets.empty(),
           "BGA preflight failure blanks only BGA and still submits every "
           "non-BGA draw");
  }

  {
    auto prepared = resources();
    RenderContext context;
    FakeBackend backend;
    rendering::SkinQuadBatchRenderer quadRenderer(backend);
    skin::Skin2DRenderer skinRenderer;
    FakeBgaSubmitter submitter;
    const skin::SkinCommandBuffer buffer{
        .frameSerial = 10,
        .commands = {
            command(1, quad(999, skin::SkinBlendMode::Normal,
                            skin::SkinFilterMode::Nearest)),
            command(2, bga(2)),
        }};
    expect(!skinRenderer.submit(buffer, prepared, context, quadRenderer, frame,
                                submitter) &&
               backend.reserveCalls == 0 && backend.batches.empty() &&
               submitter.preflightCalls == 1 &&
               submitter.commitCalls == 0 &&
               submitter.submittedTargets.empty(),
           "quad preflight failure after BGA validation consumes no BGA capacity "
           "and produces zero quad or BGA draws");
  }
}

void testLogicalScissorConversionAtOneAndTwoTimesScale() {
  rendering::render_width = 200;
  rendering::render_height = 100;
  rendering::ui_scale_x = 1.0F;
  rendering::ui_scale_y = 1.0F;
  rendering::ui_offset_x = 0;
  rendering::ui_offset_y = 0;
  const auto one = rendering::toDrawableScissor(3, 4, 20, 10);
  expect(one.enabled && one.x == 3 && one.y == 4 && one.width == 20 &&
             one.height == 10,
         "1x logical scissor converts once");

  rendering::render_width = 400;
  rendering::render_height = 200;
  rendering::ui_scale_x = 2.0F;
  rendering::ui_scale_y = 2.0F;
  const auto two = rendering::toDrawableScissor(3, 4, 20, 10);
  expect(two.enabled && two.x == 6 && two.y == 8 && two.width == 40 &&
             two.height == 20,
         "2x logical scissor converts exactly once at the backend boundary");

  const auto fractional =
      rendering::toDrawableScissor(3.25, 4.25, 20.25, 10.25);
  expect(fractional.enabled && fractional.x == 6 && fractional.y == 8 &&
             fractional.width == 41 && fractional.height == 21,
         "fractional logical edges use floor/ceil instead of field truncation");
}

void testOuterRenderContextScissorIsIntersectedBeforeSubmission() {
  auto prepared = resources();
  RenderContext context;
  context.scissor = {.x = 2, .y = 3, .width = 12, .height = 9};
  FakeBackend backend;
  rendering::SkinQuadBatchRenderer renderer(backend);
  const skin::UiLogicalRect commandClip{
      .x = 3, .y = 4, .width = 20, .height = 10};
  const std::array commands{
      command(1, quad(11, skin::SkinBlendMode::Normal,
                      skin::SkinFilterMode::Nearest, commandClip))};
  renderer.begin(context, prepared);
  expect(renderer.submit(commands), "intersecting outer scissor is valid");
  renderer.flush();
  expect(backend.batches.size() == 1 && backend.batches[0].scissor &&
             backend.batches[0].scissor->x == 3 &&
             backend.batches[0].scissor->y == 4 &&
             backend.batches[0].scissor->width == 11 &&
             backend.batches[0].scissor->height == 8,
         "command and outer context scissors intersect in UI-logical space");

  backend.batches.clear();
  context.scissor = {.x = 100, .y = 100, .width = 5, .height = 5};
  renderer.begin(context, prepared);
  expect(renderer.submit(commands),
         "an empty effective scissor safely suppresses the command");
  renderer.flush();
  expect(backend.batches.empty(),
         "an empty outer intersection emits no backend draw");

  context.scissor = {.x = 3, .y = 4, .width = 0, .height = 0};
  renderer.begin(context, prepared);
  expect(renderer.submit(commands),
         "an explicitly empty outer scissor suppresses instead of failing");
  renderer.flush();
  expect(backend.batches.empty(), "a zero-area outer scissor emits no draw");
}

void testBackendReservationFailureIsAtomic() {
  auto prepared = resources();
  RenderContext context;
  FakeBackend backend;
  backend.reserveResult = false;
  rendering::SkinQuadBatchRenderer renderer(backend);
  const std::array commands{command(1, quad(11, skin::SkinBlendMode::Normal,
                                            skin::SkinFilterMode::Nearest)),
                            command(2, quad(12, skin::SkinBlendMode::Additive,
                                            skin::SkinFilterMode::Linear))};
  renderer.begin(context, prepared);
  expect(!renderer.submit(commands),
         "insufficient whole-frame transient capacity requires fallback");
  renderer.flush();
  expect(backend.reserveCalls == 1 && backend.batches.empty(),
         "capacity failure occurs before the first backend submission");

  backend.reserveResult = true;
  backend.throwOnReserve = true;
  renderer.begin(context, prepared);
  expect(!renderer.submit(commands),
         "a fallible backend reservation exception selects fallback");
  renderer.flush();
  expect(backend.batches.empty(),
         "reservation exceptions cannot leave a partial skin frame");
}

void testVertexLayoutRegistrationFailureIsZeroDrawAtomic() {
  auto prepared = resources();
  RenderContext context;
  FakeBackend backend;
  backend.layoutPreflightResult = false;
  rendering::SkinQuadBatchRenderer renderer(backend);
  const std::array commands{command(
      1, quad(11, skin::SkinBlendMode::Normal,
              skin::SkinFilterMode::Nearest))};

  renderer.begin(context, prepared);
  expect(!renderer.submit(commands),
         "vertex-layout pool exhaustion selects fallback during preflight");
  renderer.flush();
  expect(backend.layoutPreflightCalls == 1 && backend.reserveCalls == 0 &&
             backend.batches.empty(),
         "layout registration fails before capacity reservation or submission");
}

void testVertexLayoutRegistrationRetainsAndReleasesExactHandles() {
  bgfx::VertexLayout layout;
  layout.begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .end();
  LayoutPoolControl control{.failOnCreate = 2};
  rendering::BgfxVertexLayoutRegistration registration(
      &fakeCreateVertexLayout, &fakeDestroyVertexLayout, &control);

  expect(registration.registerLayout(layout) && registration.size() == 1 &&
             control.destroyedCount == 0,
         "a validated layout handle remains registered through commit");
  expect(!registration.registerLayout(layout) && registration.size() == 1 &&
             control.destroyedCount == 0,
         "deterministic layout-pool exhaustion preserves earlier live "
         "registrations for rollback");
  auto retained = std::move(registration);
  expect(registration.size() == 0 && retained.size() == 1 &&
             control.destroyedCount == 0,
         "moving a prepared registration transfers its sole release duty");
  retained.reset();
  expect(control.destroyedCount == 1 && control.destroyed[0] == 901,
         "rollback/finalization releases every successfully registered handle "
         "exactly once");
}

void testInvalidLaterSamplerPreventsEveryBackendSubmission() {
  auto prepared = resources();
  RenderContext context;
  FakeBackend backend;
  backend.unavailableSampler = skin::SkinFilterMode::Linear;
  rendering::SkinQuadBatchRenderer renderer(backend);
  const std::array commands{
      command(1, quad(11, skin::SkinBlendMode::Normal,
                      skin::SkinFilterMode::Nearest)),
      command(2, quad(12, skin::SkinBlendMode::Additive,
                      skin::SkinFilterMode::Linear))};

  renderer.begin(context, prepared);
  expect(!renderer.submit(commands),
         "an unavailable sampler rejects the whole command span");
  renderer.flush();
  expect(backend.samplerPreflightCalls == 1 && backend.reserveCalls == 0 &&
             backend.batches.empty(),
         "a later unavailable sampler is detected before any batch submits");
}

void testLargeStripsSplitWithoutUint16IndexCorruption() {
  auto prepared = resources();
  RenderContext context;
  FakeBackend backend;
  rendering::SkinQuadBatchRenderer renderer(backend);

  skin::SkinPrimitiveCommand line{.kind = skin::SkinPrimitiveKind::LineStrip};
  line.vertices.reserve(65'540);
  for (std::size_t index = 0; index < 65'540; ++index) {
    line.vertices.push_back(
        {.x = static_cast<float>(index), .y = static_cast<float>(index & 1U)});
  }
  const std::array lineCommands{command(1, line)};
  renderer.begin(context, prepared);
  expect(renderer.submit(lineCommands), "large line strip passes preflight");
  renderer.flush();
  expect(backend.reservedVertices == 65'541 &&
             backend.reservedIndices == 65'541 && backend.batches.size() == 2 &&
             backend.batches[0].vertices.size() == 65'532 &&
             backend.batches[1].vertices.size() == 9,
         "large line strip reserves and emits two overlapped uint16 batches");
  expect(backend.batches[0].indices.back() == 65'531 &&
             backend.batches[1].indices.back() == 8 &&
             near(backend.batches[0].vertices.back().x,
                  backend.batches[1].vertices.front().x),
         "line-strip split repeats exactly one endpoint without index wrap");

  backend = FakeBackend{};
  rendering::SkinQuadBatchRenderer triangleRenderer(backend);
  line.kind = skin::SkinPrimitiveKind::TriangleStrip;
  const std::array triangleCommands{command(2, std::move(line))};
  triangleRenderer.begin(context, prepared);
  expect(triangleRenderer.submit(triangleCommands),
         "large triangle strip passes preflight");
  triangleRenderer.flush();
  expect(
      backend.reservedVertices == 65'542 && backend.reservedIndices == 65'542 &&
          backend.batches.size() == 2 &&
          backend.batches[1].vertices.size() == 10,
      "large triangle strip reserves and emits two overlapped uint16 batches");
  expect(backend.batches[0].indices.back() == 65'531 &&
             backend.batches[1].indices.back() == 9 &&
             near(backend.batches[0].vertices[65'530].x,
                  backend.batches[1].vertices[0].x) &&
             near(backend.batches[0].vertices[65'531].x,
                  backend.batches[1].vertices[1].x),
         "triangle-strip split repeats two vertices without index wrap");
}

} // namespace

int main() {
  testWholeBufferPreflightUsesExactCatalogIds();
  testVerticesStatesScissorsAndSequentialFlushes();
  testExactBackendStateMapping();
  testPrimitiveTopologyAndBgaRequiresFallbackUntilIntegrated();
  testTwoPhaseSkinSubmissionPreservesMixedAuthoredOrder();
  testBgaCompositionExpandsPlaceholdersAndMissingRolesExactly();
  testUnclippedBgaUsesProjectedSkinResolutionScissor();
  testTwoPhaseSubmissionFailuresAreZeroDrawAtomic();
  testCommittedSubmissionCannotReturnFallbackSignal();
  testLogicalScissorConversionAtOneAndTwoTimesScale();
  testOuterRenderContextScissorIsIntersectedBeforeSubmission();
  testBackendReservationFailureIsAtomic();
  testVertexLayoutRegistrationFailureIsZeroDrawAtomic();
  testVertexLayoutRegistrationRetainsAndReleasesExactHandles();
  testInvalidLaterSamplerPreventsEveryBackendSubmission();
  testLargeStripsSplitWithoutUint16IndexCorruption();
  if (failures != 0) {
    return 1;
  }
  std::cout << "Skin quad batch renderer tests passed\n";
  return 0;
}
