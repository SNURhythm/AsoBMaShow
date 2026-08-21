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

enum class SkinBatchProgram : std::uint8_t {
  Primitive,
  Textured,
  DistanceField,
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
  SkinBatchProgram program = SkinBatchProgram::Primitive;
  std::optional<skin::SkinRenderState::DistanceField> distanceField;
};

// createVertexLayout is the only bgfx API that can prove a transient vertex
// layout has a live pool entry. Holding these references across commit keeps
// allocTransientVertexBuffer's lazy lookup from becoming a post-commit
// failure. Function seams make pool exhaustion deterministic in focused tests.
class BgfxVertexLayoutRegistration final {
public:
  using Create = bgfx::VertexLayoutHandle (*)(const bgfx::VertexLayout &,
                                               void *) noexcept;
  using Destroy = void (*)(bgfx::VertexLayoutHandle, void *) noexcept;

  BgfxVertexLayoutRegistration() noexcept
      : BgfxVertexLayoutRegistration(&createDefault, &destroyDefault,
                                     nullptr) {}
  BgfxVertexLayoutRegistration(Create create, Destroy destroy,
                               void *context) noexcept
      : create_(create), destroy_(destroy), context_(context) {}
  ~BgfxVertexLayoutRegistration() { reset(); }

  BgfxVertexLayoutRegistration(BgfxVertexLayoutRegistration &&other) noexcept {
    moveFrom(other);
  }
  BgfxVertexLayoutRegistration &
  operator=(BgfxVertexLayoutRegistration &&other) noexcept {
    if (this != &other) {
      reset();
      moveFrom(other);
    }
    return *this;
  }
  BgfxVertexLayoutRegistration(const BgfxVertexLayoutRegistration &) = delete;
  BgfxVertexLayoutRegistration &
  operator=(const BgfxVertexLayoutRegistration &) = delete;

  [[nodiscard]] bool
  registerLayout(const bgfx::VertexLayout &layout) noexcept {
    if (create_ == nullptr || destroy_ == nullptr || count_ == handles_.size()) {
      return false;
    }
    const auto handle = create_(layout, context_);
    if (!bgfx::isValid(handle)) {
      return false;
    }
    handles_[count_++] = handle;
    return true;
  }

  void reset() noexcept {
    if (destroy_ != nullptr) {
      while (count_ != 0) {
        destroy_(handles_[--count_], context_);
        handles_[count_] = BGFX_INVALID_HANDLE;
      }
    } else {
      count_ = 0;
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
  static bgfx::VertexLayoutHandle
  createDefault(const bgfx::VertexLayout &layout, void *) noexcept {
    return bgfx::createVertexLayout(layout);
  }
  static void destroyDefault(bgfx::VertexLayoutHandle handle, void *) noexcept {
    bgfx::destroy(handle);
  }
  void moveFrom(BgfxVertexLayoutRegistration &other) noexcept {
    handles_ = other.handles_;
    count_ = other.count_;
    create_ = other.create_;
    destroy_ = other.destroy_;
    context_ = other.context_;
    other.count_ = 0;
    other.handles_.fill(BGFX_INVALID_HANDLE);
  }

  std::array<bgfx::VertexLayoutHandle, 4> handles_{
      bgfx::VertexLayoutHandle{bgfx::kInvalidHandle},
      bgfx::VertexLayoutHandle{bgfx::kInvalidHandle},
      bgfx::VertexLayoutHandle{bgfx::kInvalidHandle},
      bgfx::VertexLayoutHandle{bgfx::kInvalidHandle}};
  std::size_t count_ = 0;
  Create create_ = nullptr;
  Destroy destroy_ = nullptr;
  void *context_ = nullptr;
};

class SkinQuadBatchBackend {
public:
  virtual ~SkinQuadBatchBackend() = default;

  // Registers and retains every layout used by the subsequent transient
  // allocations. False is a pre-submission fallback signal.
  virtual bool preflightVertexLayouts(
      std::span<const bgfx::VertexLayout *const> layouts) = 0;

  // Called after command/resource validation and before capacity reservation.
  // Every required texture sampler must be ready before any batch can submit.
  virtual bool
  preflightSamplers(std::span<const skin::SkinFilterMode> filters) = 0;

  // Distance-field programs and uniforms are fallible GPU resources and must
  // be ready before reserve/commit. Test backends may accept them by default.
  virtual bool preflightDistanceFields(
      std::span<const skin::SkinRenderState::DistanceField>) {
    return true;
  }

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
    std::optional<skin::SkinRenderState::DistanceField> distanceField;
  };

  [[nodiscard]] bool preflightSegment(
      std::span<const skin::SkinDrawCommand> commands,
      std::vector<SkinQuadSubmissionPlan::ResolvedCommand> &resolved,
      std::vector<skin::SkinFilterMode> &samplers,
      std::vector<skin::SkinRenderState::DistanceField> &distanceFields,
      std::size_t &vertexCount,
      std::size_t &indexCount, std::size_t &skinAllocationCount) const;
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
