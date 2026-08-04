#pragma once

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

  // Called once after whole-command-buffer validation and before the first
  // submission. A false result leaves the frame completely unsubmitted.
  virtual bool reserve(std::size_t vertexCount, std::size_t indexCount) = 0;
  virtual void submit(const SkinQuadBackendBatch &) = 0;
};

[[nodiscard]] std::uint64_t skinBgfxState(skin::SkinBlendMode blend) noexcept;
[[nodiscard]] std::uint32_t
skinSamplerFlags(skin::SkinFilterMode filter) noexcept;

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

  struct ResolvedCommand {
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    std::optional<skin::UiLogicalRect> scissor;
    bool suppressed = false;
  };

  [[nodiscard]] bool preflight(std::span<const skin::SkinDrawCommand> commands,
                               std::vector<ResolvedCommand> &resolved,
                               std::size_t &vertexCount,
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
};

} // namespace rendering
