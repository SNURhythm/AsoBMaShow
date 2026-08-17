#pragma once

#include "common.h"

#include <array>
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
  std::array<UiBatchUniform, 2> uniforms{};
  std::size_t uniformCount = 0;
};

struct UiBatchSubmission {
  UiBatchVertexFormat format = UiBatchVertexFormat::Color;
  std::span<const PosColorVertex> colorVertices;
  std::span<const PosTexCoord0Vertex> texturedVertices;
  std::span<const std::uint16_t> indices;
  UiBatchState state;
};

class UiBatchBackend {
public:
  virtual ~UiBatchBackend() = default;
  virtual void beginFrame() noexcept {}
  virtual bool submit(const UiBatchSubmission &) noexcept = 0;
  virtual void shutdown() noexcept {}
};

class UiBatchRenderer final {
public:
  static constexpr std::size_t kMaximumVertices = 65'532;
  static constexpr std::size_t kMaximumIndices = 65'532;

  UiBatchRenderer();
  explicit UiBatchRenderer(UiBatchBackend &backend) noexcept;
  ~UiBatchRenderer();

  UiBatchRenderer(const UiBatchRenderer &) = delete;
  UiBatchRenderer &operator=(const UiBatchRenderer &) = delete;

  void beginFrame() noexcept;
  void begin() noexcept;
  bool appendColor(std::span<const PosColorVertex> vertices,
                   std::span<const std::uint16_t> indices,
                   const UiBatchState &state);
  bool appendTextured(std::span<const PosTexCoord0Vertex> vertices,
                      std::span<const std::uint16_t> indices,
                      const UiBatchState &state);
  bool flush() noexcept;
  void end() noexcept;
  void discard() noexcept;
  void shutdown() noexcept;

private:
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
};

} // namespace rendering
