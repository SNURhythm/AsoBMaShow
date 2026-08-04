#pragma once

#include "../audio/GameplayBgaFrame.h"
#include "../skin/beatoraja/SkinDrawCommand.h"
#include "../skin/beatoraja/SkinResourceCatalog.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct RenderContext;

namespace rendering {

class SkinQuadBatchRenderer;

struct SkinQuadGpuVertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float u = 0.0F;
  float v = 0.0F;
  std::uint32_t abgr = 0xffffffffU;

  static bgfx::VertexLayout ms_decl;
  static void init();
};

enum class SkinBatchTopology : std::uint8_t {
  Triangles,
  LineStrip,
  TriangleStrip,
};

struct SkinQuadBackendBatch {
  std::span<const SkinQuadGpuVertex> vertices;
  std::span<const std::uint16_t> indices;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  SkinBatchTopology topology = SkinBatchTopology::Triangles;
  skin::SkinBlendMode blend = skin::SkinBlendMode::Normal;
  skin::SkinFilterMode filter = skin::SkinFilterMode::Nearest;
  std::optional<skin::UiLogicalRect> scissor;
  std::uint64_t bgfxState = 0;
  std::uint32_t samplerFlags = 0;
  bool textured = false;
};

class SkinQuadBatchBackend {
public:
  virtual ~SkinQuadBatchBackend() = default;

  // Called after command/resource validation and before capacity reservation.
  // Every required texture sampler must be ready before any batch can submit.
  virtual bool
  preflightSamplers(std::span<const skin::SkinFilterMode> filters) = 0;

  // Called once after whole-command-buffer validation and before the first
  // submission. A false result leaves the frame completely unsubmitted.
  virtual bool reserve(
      std::size_t vertexCount, std::size_t indexCount,
      std::size_t skinAllocationCount,
      const GameplayBgaTransientRequirements &bgaRequirements) = 0;
  virtual void submit(const SkinQuadBackendBatch &) = 0;
};

[[nodiscard]] std::uint64_t skinBgfxState(skin::SkinBlendMode blend) noexcept;
[[nodiscard]] std::uint32_t
skinSamplerFlags(skin::SkinFilterMode filter) noexcept;

// A value-owned preflight result for non-BGA command spans. Command payloads
// remain borrowed from the immutable SkinCommandBuffer for the duration of
// one Skin2DRenderer::submit call; every resource lookup, allocation, sampler
// check, and backend reservation is completed before this plan becomes ready.
class SkinQuadSubmissionPlan final {
public:
  SkinQuadSubmissionPlan() = default;
  SkinQuadSubmissionPlan(SkinQuadSubmissionPlan &&) noexcept = default;
  SkinQuadSubmissionPlan &
  operator=(SkinQuadSubmissionPlan &&) noexcept = default;
  SkinQuadSubmissionPlan(const SkinQuadSubmissionPlan &) = delete;
  SkinQuadSubmissionPlan &operator=(const SkinQuadSubmissionPlan &) = delete;

  [[nodiscard]] std::size_t segmentCount() const noexcept {
    return segments_.size();
  }
  [[nodiscard]] bool fullyConsumed() const noexcept {
    return ready_ && nextSegment_ == segments_.size();
  }

private:
  struct ResolvedCommand {
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    std::optional<skin::UiLogicalRect> scissor;
    bool suppressed = false;
  };

  struct Segment {
    std::span<const skin::SkinDrawCommand> commands;
    std::vector<ResolvedCommand> resolved;
  };

  const SkinQuadBatchRenderer *owner_ = nullptr;
  std::uint64_t generation_ = 0;
  std::size_t nextSegment_ = 0;
  std::vector<Segment> segments_;
  bool ready_ = false;

  friend class SkinQuadBatchRenderer;
};

class SkinQuadBatchRenderer final {
public:
  SkinQuadBatchRenderer();
  explicit SkinQuadBatchRenderer(SkinQuadBatchBackend &backend);
  ~SkinQuadBatchRenderer();

  SkinQuadBatchRenderer(const SkinQuadBatchRenderer &) = delete;
  SkinQuadBatchRenderer &operator=(const SkinQuadBatchRenderer &) = delete;

  void begin(RenderContext &, const skin::SkinResourceCatalog &);

  // The interface form keeps backend-state and golden tests independent from
  // resource upload while production still binds a concrete session catalog.
  void begin(RenderContext &, const skin::SkinPreparedResourceView &);

  // Two-phase path used by the skin/BGA compositor. BGA variants must never
  // appear in these spans. All spans are preflighted together so aggregate
  // transient capacity is known before any backend submission.
  [[nodiscard]] bool prepare(
      std::span<const std::span<const skin::SkinDrawCommand>> segments,
      SkinQuadSubmissionPlan &plan,
      const GameplayBgaTransientRequirements &bgaRequirements = {});
  void submitPrepared(SkinQuadSubmissionPlan &plan,
                      std::size_t segmentIndex) noexcept;
  void discardPrepared(SkinQuadSubmissionPlan &plan) noexcept;

  // Legacy adapter: one non-BGA span is prepared and emitted internally.
  [[nodiscard]] bool submit(std::span<const skin::SkinDrawCommand> commands);
  void flush();

private:
  struct BatchKey {
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    SkinBatchTopology topology = SkinBatchTopology::Triangles;
    skin::SkinBlendMode blend = skin::SkinBlendMode::Normal;
    skin::SkinFilterMode filter = skin::SkinFilterMode::Nearest;
    std::optional<skin::UiLogicalRect> scissor;
    bool textured = false;
  };

  [[nodiscard]] bool preflightSegment(
      std::span<const skin::SkinDrawCommand> commands,
      std::vector<SkinQuadSubmissionPlan::ResolvedCommand> &resolved,
      std::vector<skin::SkinFilterMode> &samplers, std::size_t &vertexCount,
      std::size_t &indexCount) const;
  void appendQuad(const std::array<skin::SkinVertex, 4> &, const BatchKey &);
  void appendPrimitive(const skin::SkinPrimitiveCommand &, const BatchKey &);
  void requireBatch(const BatchKey &);
  void flushBatch();
  void clearBatch() noexcept;

  std::unique_ptr<SkinQuadBatchBackend> ownedBackend_;
  SkinQuadBatchBackend *backend_ = nullptr;
  const skin::SkinPreparedResourceView *resources_ = nullptr;
  std::optional<skin::UiLogicalRect> outerScissor_;
  std::optional<BatchKey> batchKey_;
  std::vector<SkinQuadGpuVertex> vertices_;
  std::vector<std::uint16_t> indices_;
  bool ready_ = false;
  bool submittedSpan_ = false;
  std::uint64_t generation_ = 0;
};

} // namespace rendering
