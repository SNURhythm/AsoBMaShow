#include "SkinQuadBatchRenderer.h"

#include "RenderPlan.h"
#include "ShaderManager.h"
#include "UniformCache.h"
#include "common.h"
#include "../view/View.h"

#include <bx/bx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace rendering {
namespace {

constexpr std::size_t maximumBatchVertices = 65'532;
constexpr std::size_t maximumBatchIndices = 65'532;

std::size_t chunkedStripSize(std::size_t vertices,
                             std::size_t overlap) noexcept {
  if (vertices <= maximumBatchVertices) {
    return vertices;
  }
  const std::size_t newVerticesPerChunk = maximumBatchVertices - overlap;
  const std::size_t remaining = vertices - maximumBatchVertices;
  const std::size_t extraChunks =
      (remaining + newVerticesPerChunk - 1U) / newVerticesPerChunk;
  return vertices + extraChunks * overlap;
}

bool finite(float value) noexcept { return std::isfinite(value); }
bool finite(double value) noexcept { return std::isfinite(value); }

bool sameScissor(const std::optional<skin::UiLogicalRect> &left,
                 const std::optional<skin::UiLogicalRect> &right) noexcept {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left ||
         (left->x == right->x && left->y == right->y &&
          left->width == right->width && left->height == right->height);
}

bool validVertex(const skin::SkinVertex &vertex) noexcept {
  return finite(vertex.x) && finite(vertex.y) && finite(vertex.u) &&
         finite(vertex.v);
}

std::optional<skin::UiLogicalRect>
intersectScissors(const std::optional<skin::UiLogicalRect> &command,
                  const std::optional<skin::UiLogicalRect> &outer,
                  bool &empty) noexcept {
  empty = false;
  if (outer && (outer->width <= 0.0 || outer->height <= 0.0)) {
    empty = true;
    return std::nullopt;
  }
  if (!command) {
    return outer;
  }
  if (!outer) {
    return command;
  }
  const double left = std::max(command->x, outer->x);
  const double top = std::max(command->y, outer->y);
  const double right =
      std::min(command->x + command->width, outer->x + outer->width);
  const double bottom =
      std::min(command->y + command->height, outer->y + outer->height);
  if (right <= left || bottom <= top) {
    empty = true;
    return std::nullopt;
  }
  return skin::UiLogicalRect{
      .x = left, .y = top, .width = right - left, .height = bottom - top};
}

bool validScissor(const std::optional<skin::UiLogicalRect> &scissor) noexcept {
  return !scissor ||
         (finite(scissor->x) && finite(scissor->y) && finite(scissor->width) &&
          finite(scissor->height) && scissor->width > 0.0 &&
          scissor->height > 0.0 &&
          std::abs(scissor->x) <=
              static_cast<double>(std::numeric_limits<int>::max() / 4) &&
          std::abs(scissor->y) <=
              static_cast<double>(std::numeric_limits<int>::max() / 4) &&
          scissor->width <=
              static_cast<double>(std::numeric_limits<int>::max() / 4) &&
          scissor->height <=
              static_cast<double>(std::numeric_limits<int>::max() / 4));
}

const bgfx::VertexLayout &transientCapacityProbeLayout() {
  static const bgfx::VertexLayout layout = [] {
    bgfx::VertexLayout value;
    value.begin()
        .add(bgfx::Attrib::Position, 1, bgfx::AttribType::Float)
        .end();
    return value;
  }();
  return layout;
}

class BgfxSkinQuadBatchBackend final : public SkinQuadBatchBackend {
public:
  bool preflightSamplers(
      std::span<const skin::SkinFilterMode> filters) override {
    if (filters.empty()) {
      return true;
    }
    try {
      sampler_ = UniformCache::getInstance().getSampler("s_texColor");
    } catch (...) {
      return false;
    }
    return bgfx::isValid(sampler_);
  }

  bool reserve(
      std::size_t vertexCount, std::size_t indexCount,
      std::size_t skinAllocationCount,
      const GameplayBgaTransientRequirements &bgaRequirements) override {
    if (vertexCount != 0 || indexCount != 0) {
      try {
        // Program creation is part of the no-draw preflight. A missing
        // packaged shader therefore selects fallback before any submission.
        texturedProgram_ = ShaderManager::getInstance().getProgram(
            "vs_skin_quad.bin", "fs_skin_quad.bin");
        primitiveProgram_ =
            ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
      } catch (...) {
        return false;
      }
      if (!bgfx::isValid(texturedProgram_) ||
          !bgfx::isValid(primitiveProgram_)) {
        return false;
      }
    }

    const auto skinStride =
        static_cast<std::uint64_t>(SkinQuadGpuVertex::ms_decl.getStride());
    if (skinStride == 0 ||
        static_cast<std::uint64_t>(vertexCount) >
            std::numeric_limits<std::uint64_t>::max() / skinStride) {
      return false;
    }
    std::uint64_t requiredVertexBytes =
        static_cast<std::uint64_t>(vertexCount) * skinStride;
    const auto paddingPerSkinAllocation = skinStride - 1U;
    if (paddingPerSkinAllocation != 0 &&
        static_cast<std::uint64_t>(skinAllocationCount) >
            std::numeric_limits<std::uint64_t>::max() /
                paddingPerSkinAllocation) {
      return false;
    }
    const auto skinAlignmentPadding =
        static_cast<std::uint64_t>(skinAllocationCount) *
        paddingPerSkinAllocation;
    const auto addVertexBytes = [&](std::uint64_t bytes) {
      if (bytes > std::numeric_limits<std::uint64_t>::max() -
                      requiredVertexBytes) {
        return false;
      }
      requiredVertexBytes += bytes;
      return true;
    };
    if (!addVertexBytes(skinAlignmentPadding) ||
        !addVertexBytes(bgaRequirements.vertexBytes) ||
        !addVertexBytes(bgaRequirements.vertexAlignmentPadding)) {
      return false;
    }

    if (bgaRequirements.indexCount >
        std::numeric_limits<std::uint64_t>::max() -
            static_cast<std::uint64_t>(indexCount)) {
      return false;
    }
    const auto requiredIndices =
        static_cast<std::uint64_t>(indexCount) + bgaRequirements.indexCount;
    if (requiredIndices > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }

    const auto *caps = bgfx::getCaps();
    if (caps == nullptr) {
      return requiredVertexBytes == 0 && requiredIndices == 0;
    }
    const auto &probeLayout = transientCapacityProbeLayout();
    const auto probeStride =
        static_cast<std::uint64_t>(probeLayout.getStride());
    if (probeStride == 0) {
      return false;
    }
    const auto maximumProbeVertices = static_cast<std::uint32_t>(
        caps->limits.maxTransientVbSize / probeStride);
    const auto availableVertexBytes =
        static_cast<std::uint64_t>(bgfx::getAvailTransientVertexBuffer(
            maximumProbeVertices, probeLayout)) *
        probeStride;
    const auto indices = static_cast<std::uint32_t>(requiredIndices);
    return requiredVertexBytes <= availableVertexBytes &&
           bgfx::getAvailTransientIndexBuffer(indices) >= indices;
  }

  void submit(const SkinQuadBackendBatch &batch) override {
    if (batch.vertices.empty() || batch.indices.empty()) {
      return;
    }
    bgfx::TransientVertexBuffer vertexBuffer;
    bgfx::TransientIndexBuffer indexBuffer;
    bgfx::allocTransientVertexBuffer(
        &vertexBuffer, static_cast<std::uint32_t>(batch.vertices.size()),
        SkinQuadGpuVertex::ms_decl);
    bgfx::allocTransientIndexBuffer(
        &indexBuffer, static_cast<std::uint32_t>(batch.indices.size()));
    bx::memCopy(vertexBuffer.data, batch.vertices.data(),
                batch.vertices.size_bytes());
    bx::memCopy(indexBuffer.data, batch.indices.data(),
                batch.indices.size_bytes());
    bgfx::setVertexBuffer(0, &vertexBuffer);
    bgfx::setIndexBuffer(&indexBuffer);
    if (batch.textured) {
      bgfx::setTexture(0, sampler_, batch.texture, batch.samplerFlags);
    }
    std::uint64_t state = batch.bgfxState;
    switch (batch.topology) {
    case SkinBatchTopology::Triangles:
      break;
    case SkinBatchTopology::LineStrip:
      state |= BGFX_STATE_PT_LINESTRIP;
      break;
    case SkinBatchTopology::TriangleStrip:
      state |= BGFX_STATE_PT_TRISTRIP;
      break;
    }
    bgfx::setState(state);
    if (batch.scissor) {
      rendering::setScissorUI(batch.scissor->x, batch.scissor->y,
                              batch.scissor->width, batch.scissor->height);
    } else {
      bgfx::setScissor();
    }
    bgfx::submit(rendering::ui_view,
                 batch.textured ? texturedProgram_ : primitiveProgram_);
  }

private:
  bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle texturedProgram_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle primitiveProgram_ = BGFX_INVALID_HANDLE;
};

} // namespace

bgfx::VertexLayout SkinQuadGpuVertex::ms_decl;

void SkinQuadGpuVertex::init() {
  ms_decl.begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
      .end();
}

std::uint64_t skinBgfxState(skin::SkinBlendMode blend) noexcept {
  constexpr std::uint64_t base = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
  switch (blend) {
  case skin::SkinBlendMode::Normal:
    return base | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                        BGFX_STATE_BLEND_INV_SRC_ALPHA);
  case skin::SkinBlendMode::Additive:
    return base | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                        BGFX_STATE_BLEND_ONE);
  case skin::SkinBlendMode::Subtractive:
    // Beatoraja's pinned renderer intends SRC_ALPHA/ONE with subtraction.
    // Its current state ordering restores ADD before drawing this sprite; we
    // deliberately implement the intended stable state rather than that leak.
    return base |
           BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                 BGFX_STATE_BLEND_ONE) |
           BGFX_STATE_BLEND_EQUATION(BGFX_STATE_BLEND_EQUATION_SUB);
  case skin::SkinBlendMode::Multiply:
    return base | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ZERO,
                                        BGFX_STATE_BLEND_SRC_COLOR);
  case skin::SkinBlendMode::Inverse:
    return base | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_INV_DST_COLOR,
                                        BGFX_STATE_BLEND_ZERO);
  }
  return base;
}

std::uint32_t skinSamplerFlags(skin::SkinFilterMode filter) noexcept {
  const std::uint32_t clamp = BGFX_SAMPLER_UVW_CLAMP;
  return filter == skin::SkinFilterMode::Nearest ? clamp | BGFX_SAMPLER_POINT
                                                 : clamp;
}

SkinQuadBatchRenderer::SkinQuadBatchRenderer()
    : ownedBackend_(std::make_unique<BgfxSkinQuadBatchBackend>()),
      backend_(ownedBackend_.get()) {
  SkinQuadGpuVertex::init();
  vertices_.reserve(4096);
  indices_.reserve(6144);
}

SkinQuadBatchRenderer::SkinQuadBatchRenderer(SkinQuadBatchBackend &backend)
    : backend_(&backend) {
  SkinQuadGpuVertex::init();
  vertices_.reserve(4096);
  indices_.reserve(6144);
}

SkinQuadBatchRenderer::~SkinQuadBatchRenderer() = default;

void SkinQuadBatchRenderer::begin(RenderContext &context,
                                  const skin::SkinResourceCatalog &catalog) {
  begin(context, static_cast<const skin::SkinPreparedResourceView &>(catalog));
}

void SkinQuadBatchRenderer::begin(
    RenderContext &context, const skin::SkinPreparedResourceView &resources) {
  clearBatch();
  if (++generation_ == 0) {
    ++generation_;
  }
  resources_ = &resources;
  submittedSpan_ = false;
  ready_ = backend_ != nullptr;
  outerScissor_.reset();
  if (context.scissor.width >= 0 && context.scissor.height >= 0) {
    outerScissor_ = skin::UiLogicalRect{
        .x = static_cast<double>(context.scissor.x),
        .y = static_cast<double>(context.scissor.y),
        .width = static_cast<double>(context.scissor.width),
        .height = static_cast<double>(context.scissor.height)};
    if (outerScissor_->width > 0.0 && outerScissor_->height > 0.0 &&
        !validScissor(outerScissor_)) {
      ready_ = false;
    }
  }
}

bool SkinQuadBatchRenderer::preflightSegment(
    std::span<const skin::SkinDrawCommand> commands,
    std::vector<SkinQuadSubmissionPlan::ResolvedCommand> &resolved,
    std::vector<skin::SkinFilterMode> &samplers, std::size_t &vertexCount,
    std::size_t &indexCount) const {
  resolved.clear();
  resolved.resize(commands.size());
  vertexCount = 0;
  indexCount = 0;
  const auto addCounts = [&](std::size_t vertices, std::size_t indices) {
    if (vertices > std::numeric_limits<std::size_t>::max() - vertexCount ||
        indices > std::numeric_limits<std::size_t>::max() - indexCount) {
      return false;
    }
    vertexCount += vertices;
    indexCount += indices;
    return true;
  };
  const auto addSampler = [&](skin::SkinFilterMode filter) {
    if (std::ranges::find(samplers, filter) == samplers.end()) {
      samplers.push_back(filter);
    }
  };

  for (std::size_t commandIndex = 0; commandIndex < commands.size();
       ++commandIndex) {
    const auto &command = commands[commandIndex];
    auto &prepared = resolved[commandIndex];
    const auto resolveScissor = [&](const skin::SkinRenderState &state) {
      if (!validScissor(state.scissor)) {
        return false;
      }
      bool empty = false;
      prepared.scissor = intersectScissors(state.scissor, outerScissor_, empty);
      prepared.suppressed = empty;
      return !prepared.scissor || validScissor(prepared.scissor);
    };

    if (const auto *quad =
            std::get_if<skin::SkinTexturedQuadCommand>(&command.payload)) {
      const auto *resource = resources_->find(quad->resource);
      if (!resource || !bgfx::isValid(resource->texture) ||
          !std::ranges::all_of(quad->vertices, validVertex) ||
          !resolveScissor(quad->state)) {
        return false;
      }
      prepared.texture = resource->texture;
      if (!prepared.suppressed && !addCounts(4, 6)) {
        return false;
      }
      if (!prepared.suppressed) {
        addSampler(quad->state.filter);
      }
      continue;
    }
    if (const auto *glyphs =
            std::get_if<skin::SkinGlyphRunCommand>(&command.payload)) {
      const auto *atlas = resources_->findTextAtlas(glyphs->atlas);
      if (!atlas || !bgfx::isValid(atlas->texture) ||
          !resolveScissor(glyphs->state)) {
        return false;
      }
      prepared.texture = atlas->texture;
      for (const auto &glyph : glyphs->glyphs) {
        if (!atlas->glyphs.contains(glyph.codepoint) ||
            !std::ranges::all_of(glyph.vertices, validVertex)) {
          return false;
        }
      }
      if (!prepared.suppressed &&
          (glyphs->glyphs.size() >
               std::numeric_limits<std::size_t>::max() / 6U ||
           !addCounts(glyphs->glyphs.size() * 4U,
                      glyphs->glyphs.size() * 6U))) {
        return false;
      }
      if (!prepared.suppressed && !glyphs->glyphs.empty()) {
        addSampler(glyphs->state.filter);
      }
      continue;
    }
    if (const auto *primitive =
            std::get_if<skin::SkinPrimitiveCommand>(&command.payload)) {
      if (!resolveScissor(primitive->state) ||
          !std::ranges::all_of(primitive->vertices, validVertex)) {
        return false;
      }
      std::size_t vertices = primitive->vertices.size();
      std::size_t indices = vertices;
      switch (primitive->kind) {
      case skin::SkinPrimitiveKind::SolidQuad:
        if (vertices != 4) {
          return false;
        }
        indices = 6;
        break;
      case skin::SkinPrimitiveKind::LineStrip:
        if (vertices < 2) {
          return false;
        }
        vertices = chunkedStripSize(vertices, 1);
        indices = vertices;
        break;
      case skin::SkinPrimitiveKind::TriangleStrip:
        if (vertices < 3) {
          return false;
        }
        vertices = chunkedStripSize(vertices, 2);
        indices = vertices;
        break;
      }
      if (!prepared.suppressed && !addCounts(vertices, indices)) {
        return false;
      }
      continue;
    }
    // BGA has an explicit authored-order slot, but its video/frame resolver
    // is integrated later. Silently skipping it would produce incomplete
    // content, so the entire skin buffer falls back until that bridge exists.
    return false;
  }
  return true;
}

bool SkinQuadBatchRenderer::prepare(
    std::span<const std::span<const skin::SkinDrawCommand>> segments,
    SkinQuadSubmissionPlan &plan,
    const GameplayBgaTransientRequirements &bgaRequirements) {
  if (!ready_ || submittedSpan_ || !resources_) {
    return false;
  }
  submittedSpan_ = true;
  plan = SkinQuadSubmissionPlan{};
  std::vector<skin::SkinFilterMode> samplers;
  std::size_t totalVertexCount = 0;
  std::size_t totalIndexCount = 0;
  std::size_t skinAllocationCount = 0;
  std::size_t maximumSegmentVertices = 0;
  std::size_t maximumSegmentIndices = 0;
  try {
    plan.segments_.reserve(segments.size());
    for (const auto commands : segments) {
      SkinQuadSubmissionPlan::Segment segment{.commands = commands};
      std::size_t vertexCount = 0;
      std::size_t indexCount = 0;
      if (!preflightSegment(commands, segment.resolved, samplers, vertexCount,
                            indexCount) ||
          vertexCount >
              std::numeric_limits<std::size_t>::max() - totalVertexCount ||
          indexCount >
              std::numeric_limits<std::size_t>::max() - totalIndexCount) {
        ready_ = false;
        clearBatch();
        return false;
      }
      totalVertexCount += vertexCount;
      totalIndexCount += indexCount;
      if (vertexCount != 0) {
        ++skinAllocationCount;
      }
      maximumSegmentVertices =
          std::max(maximumSegmentVertices, vertexCount);
      maximumSegmentIndices = std::max(maximumSegmentIndices, indexCount);
      plan.segments_.push_back(std::move(segment));
    }
    // No CPU allocation is allowed after this preparation point: state
    // changes can flush immediately around BGA slots, so all working buffers
    // retain enough capacity for the largest preflighted segment chunk.
    vertices_.reserve(
        std::min(maximumSegmentVertices, maximumBatchVertices));
    indices_.reserve(std::min(maximumSegmentIndices, maximumBatchIndices));
    if ((!segments.empty() || !bgaRequirements.empty()) &&
        (!backend_->preflightSamplers(samplers) ||
         !backend_->reserve(totalVertexCount, totalIndexCount,
                            skinAllocationCount,
                            bgaRequirements))) {
      ready_ = false;
      clearBatch();
      return false;
    }
  } catch (...) {
    // Every fallible operation happens before backend submission. Allocation,
    // shader-loading, and backend-reservation failures therefore select
    // built-in fallback without leaving a hybrid partial skin frame.
    ready_ = false;
    clearBatch();
    return false;
  }

  plan.owner_ = this;
  plan.generation_ = generation_;
  plan.ready_ = true;
  return true;
}

void SkinQuadBatchRenderer::submitPrepared(SkinQuadSubmissionPlan &plan,
                                            std::size_t segmentIndex) noexcept {
  if (!ready_ || !plan.ready_ || plan.owner_ != this ||
      plan.generation_ != generation_ || segmentIndex != plan.nextSegment_ ||
      segmentIndex >= plan.segments_.size()) {
    discardPrepared(plan);
    return;
  }

  const auto &segment = plan.segments_[segmentIndex];
  const auto commands = segment.commands;
  const auto &resolved = segment.resolved;

  for (std::size_t commandIndex = 0; commandIndex < commands.size();
       ++commandIndex) {
    if (resolved[commandIndex].suppressed) {
      continue;
    }
    const auto &command = commands[commandIndex];
    if (const auto *quad =
            std::get_if<skin::SkinTexturedQuadCommand>(&command.payload)) {
      appendQuad(quad->vertices, {.texture = resolved[commandIndex].texture,
                                  .topology = SkinBatchTopology::Triangles,
                                  .blend = quad->state.blend,
                                  .filter = quad->state.filter,
                                  .scissor = resolved[commandIndex].scissor,
                                  .textured = true});
      continue;
    }
    if (const auto *glyphs =
            std::get_if<skin::SkinGlyphRunCommand>(&command.payload)) {
      const BatchKey key{.texture = resolved[commandIndex].texture,
                         .topology = SkinBatchTopology::Triangles,
                         .blend = glyphs->state.blend,
                         .filter = glyphs->state.filter,
                         .scissor = resolved[commandIndex].scissor,
                         .textured = true};
      for (const auto &glyph : glyphs->glyphs) {
        appendQuad(glyph.vertices, key);
      }
      continue;
    }
    const auto &primitive =
        std::get<skin::SkinPrimitiveCommand>(command.payload);
    const auto topology =
        primitive.kind == skin::SkinPrimitiveKind::LineStrip
            ? SkinBatchTopology::LineStrip
        : primitive.kind == skin::SkinPrimitiveKind::TriangleStrip
            ? SkinBatchTopology::TriangleStrip
            : SkinBatchTopology::Triangles;
    appendPrimitive(primitive, {.topology = topology,
                                .blend = primitive.state.blend,
                                .filter = primitive.state.filter,
                                .scissor = resolved[commandIndex].scissor,
                                .textured = false});
  }
  ++plan.nextSegment_;
}

void SkinQuadBatchRenderer::discardPrepared(
    SkinQuadSubmissionPlan &plan) noexcept {
  if (plan.owner_ == this && plan.generation_ == generation_) {
    plan.ready_ = false;
    ready_ = false;
    clearBatch();
  }
}

bool SkinQuadBatchRenderer::submit(
    std::span<const skin::SkinDrawCommand> commands) {
  const std::array segments{commands};
  SkinQuadSubmissionPlan plan;
  if (!prepare(segments, plan)) {
    return false;
  }
  submitPrepared(plan, 0);
  return ready_ && plan.fullyConsumed();
}

void SkinQuadBatchRenderer::requireBatch(const BatchKey &key) {
  const bool compatible =
      batchKey_ && batchKey_->texture.idx == key.texture.idx &&
      batchKey_->topology == key.topology && batchKey_->blend == key.blend &&
      batchKey_->filter == key.filter && batchKey_->textured == key.textured &&
      sameScissor(batchKey_->scissor, key.scissor);
  if (!compatible) {
    flushBatch();
    batchKey_ = key;
  }
}

void SkinQuadBatchRenderer::appendQuad(
    const std::array<skin::SkinVertex, 4> &quad, const BatchKey &key) {
  requireBatch(key);
  if (vertices_.size() + 4U > maximumBatchVertices ||
      indices_.size() + 6U > maximumBatchIndices) {
    flushBatch();
    batchKey_ = key;
  }
  const auto first = static_cast<std::uint16_t>(vertices_.size());
  for (const auto &vertex : quad) {
    vertices_.push_back({.x = vertex.x,
                         .y = vertex.y,
                         .u = vertex.u,
                         .v = vertex.v,
                         .abgr = vertex.rgba});
  }
  indices_.insert(indices_.end(), {static_cast<std::uint16_t>(first + 0U),
                                   static_cast<std::uint16_t>(first + 1U),
                                   static_cast<std::uint16_t>(first + 2U),
                                   static_cast<std::uint16_t>(first + 0U),
                                   static_cast<std::uint16_t>(first + 2U),
                                   static_cast<std::uint16_t>(first + 3U)});
}

void SkinQuadBatchRenderer::appendPrimitive(
    const skin::SkinPrimitiveCommand &primitive, const BatchKey &key) {
  if (primitive.kind == skin::SkinPrimitiveKind::SolidQuad) {
    std::array<skin::SkinVertex, 4> quad;
    std::copy_n(primitive.vertices.begin(), 4, quad.begin());
    appendQuad(quad, key);
    return;
  }
  flushBatch();
  const std::size_t overlap =
      primitive.kind == skin::SkinPrimitiveKind::LineStrip ? 1U : 2U;
  std::size_t consumed = 0;
  while (consumed < primitive.vertices.size()) {
    const std::size_t first = consumed == 0 ? 0 : consumed - overlap;
    const std::size_t count =
        std::min(maximumBatchVertices, primitive.vertices.size() - first);
    batchKey_ = key;
    for (std::size_t index = 0; index < count; ++index) {
      const auto &vertex = primitive.vertices[first + index];
      vertices_.push_back({.x = vertex.x,
                           .y = vertex.y,
                           .u = vertex.u,
                           .v = vertex.v,
                           .abgr = vertex.rgba});
      indices_.push_back(static_cast<std::uint16_t>(index));
    }
    consumed = first + count;
    flushBatch();
  }
}

void SkinQuadBatchRenderer::flushBatch() {
  if (!batchKey_ || vertices_.empty() || indices_.empty()) {
    clearBatch();
    return;
  }
  backend_->submit({.vertices = vertices_,
                    .indices = indices_,
                    .texture = batchKey_->texture,
                    .topology = batchKey_->topology,
                    .blend = batchKey_->blend,
                    .filter = batchKey_->filter,
                    .scissor = batchKey_->scissor,
                    .bgfxState = skinBgfxState(batchKey_->blend),
                    .samplerFlags = skinSamplerFlags(batchKey_->filter),
                    .textured = batchKey_->textured});
  clearBatch();
}

void SkinQuadBatchRenderer::clearBatch() noexcept {
  vertices_.clear();
  indices_.clear();
  batchKey_.reset();
}

void SkinQuadBatchRenderer::flush() {
  if (!ready_) {
    clearBatch();
    return;
  }
  flushBatch();
}

} // namespace rendering
