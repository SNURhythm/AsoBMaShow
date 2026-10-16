#pragma once

#include "common.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace rendering {

enum class UiBatchVertexFormat : std::uint8_t { Color, Textured };

struct UiBatchScissor {
  int x = 0;
  int y = 0;
  int width = -1;
  int height = -1;
};

struct UiBatchUniform {
  bgfx::UniformHandle handle = BGFX_INVALID_HANDLE;
  std::array<float, 4> value{};
};

struct UiBatchState {
  bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
  std::uint32_t samplerFlags = std::numeric_limits<std::uint32_t>::max();
  std::uint64_t state = 0;
  std::optional<UiBatchScissor> scissor;
  std::optional<std::array<float, 16>> transform;
  std::array<UiBatchUniform, 6> uniforms{};
  std::size_t uniformCount = 0;
  float textureOpacity = 1.0f;
};

struct UiBatchSubmission {
  UiBatchVertexFormat format = UiBatchVertexFormat::Color;
  std::span<const PosColorVertex> colorVertices;
  std::span<const PosTexCoord0Vertex> texturedVertices;
  std::span<const std::uint16_t> indices;
  UiBatchState state;
};

// Immutable geometry can stay in GPU buffers while only draw uniforms change.
// Owners replace this object when the image/layout changes; the backend holds
// a weak reference so it can retire the associated buffers after replacement.
struct UiRetainedGeometry {
  bgfx::VertexLayout layout;
  std::vector<std::byte> vertices;
  std::vector<std::uint16_t> indices;
};

class UiBatchBackend {
public:
  virtual ~UiBatchBackend() = default;
  virtual void beginFrame() noexcept {}
  virtual bool submit(const UiBatchSubmission &) noexcept = 0;
  virtual bool submitRetained(const std::shared_ptr<const UiRetainedGeometry> &,
                              const UiBatchState &) noexcept { return false; }
  virtual void shutdown() noexcept {}
};

class UiBatchRenderer final {
public:
  static constexpr std::size_t kMaximumVertices = 65'532;
  static constexpr std::size_t kMaximumIndices = 65'532;
  static constexpr std::size_t kMaximumDeferredBytes = 8 * 1024 * 1024;

  UiBatchRenderer();
  explicit UiBatchRenderer(UiBatchBackend &backend) noexcept;
  ~UiBatchRenderer();

  UiBatchRenderer(const UiBatchRenderer &) = delete;
  UiBatchRenderer &operator=(const UiBatchRenderer &) = delete;

  void beginFrame(bool deferSubmission = false) noexcept;
  // Returns false only when a deferred frame exactly matches the last one.
  // Callers must invalidate it if presentation is interrupted or discarded.
  bool finishFrame(bool forcePresentation = false) noexcept;
  void invalidateFrame() noexcept { previousFrameValid_ = false; }
  static void notifyTextureChanged() noexcept {
    textureRevision_.fetch_add(1, std::memory_order_relaxed);
  }
  void begin() noexcept;
  bool appendColor(std::span<const PosColorVertex> vertices,
                   std::span<const std::uint16_t> indices,
                   const UiBatchState &state);
  bool appendTextured(std::span<const PosTexCoord0Vertex> vertices,
                      std::span<const std::uint16_t> indices,
                      const UiBatchState &state);
  bool submitRetained(const std::shared_ptr<const UiRetainedGeometry> &geometry,
                      const UiBatchState &state) noexcept;
  bool flush() noexcept;
  void end() noexcept;
  void discard() noexcept;
  void shutdown() noexcept;

private:
  struct RecordedDraw {
    UiBatchVertexFormat format = UiBatchVertexFormat::Color;
    std::size_t vertexOffset = 0, vertexCount = 0;
    std::size_t indexOffset = 0, indexCount = 0;
    UiBatchState state;
  };
  struct RecordedFrame {
    std::vector<RecordedDraw> draws;
    std::vector<PosColorVertex> colors;
    std::vector<PosTexCoord0Vertex> textures;
    std::vector<std::uint16_t> indices;
    void clear() noexcept;
    bool sameAs(const RecordedFrame &) const noexcept;
  };
  bool recordSubmission(const UiBatchSubmission &) noexcept;
  bool submitRecorded() noexcept;
  void useImmediateSubmission() noexcept;
  template <typename Vertex>
  bool append(UiBatchVertexFormat format, std::span<const Vertex> vertices,
              std::span<const std::uint16_t> indices,
              const UiBatchState &state);
  bool flushCurrent() noexcept;
  void clearCurrent() noexcept;

  std::unique_ptr<UiBatchBackend> ownedBackend_;
  UiBatchBackend *backend_ = nullptr;
  std::optional<UiBatchVertexFormat> format_;
  std::optional<UiBatchState> state_;
  std::vector<PosColorVertex> colorVertices_;
  std::vector<PosTexCoord0Vertex> texturedVertices_;
  std::vector<std::uint16_t> indices_;
  bool active_ = false;
  RecordedFrame recordedFrame_, previousFrame_;
  std::size_t recordedBytes_ = 0;
  bool deferredSubmission_ = false;
  bool previousFrameValid_ = false;
  std::uint64_t previousTextureRevision_ = 0;
  static inline std::atomic<std::uint64_t> textureRevision_{0};
};

} // namespace rendering
