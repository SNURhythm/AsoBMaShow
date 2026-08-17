#include "UiBatchRenderer.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <type_traits>
#include <utility>

namespace rendering {
namespace {

bool sameScissor(const std::optional<UiBatchScissor> &left,
                 const std::optional<UiBatchScissor> &right) noexcept {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left ||
         (left->x == right->x && left->y == right->y &&
          left->width == right->width && left->height == right->height);
}

bool sameUniform(const UiBatchUniform &left,
                 const UiBatchUniform &right) noexcept {
  return left.handle.idx == right.handle.idx && left.value == right.value;
}

bool sameState(const UiBatchState &left, const UiBatchState &right) noexcept {
  if (left.program.idx != right.program.idx ||
      left.texture.idx != right.texture.idx ||
      left.sampler.idx != right.sampler.idx ||
      left.samplerFlags != right.samplerFlags || left.state != right.state ||
      !sameScissor(left.scissor, right.scissor) ||
      left.transform != right.transform || left.uniformCount != right.uniformCount ||
      left.uniformCount > left.uniforms.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.uniformCount; ++index) {
    if (!sameUniform(left.uniforms[index], right.uniforms[index])) {
      return false;
    }
  }
  return true;
}

class BgfxUiBatchBackend final : public UiBatchBackend {
public:
  BgfxUiBatchBackend() {
    PosColorVertex::init();
    PosTexCoord0Vertex::init();
  }

  ~BgfxUiBatchBackend() override { shutdown(); }

  void beginFrame() noexcept override {
    for (auto &page : bufferPages_) {
      page.colorVertexCount = 0;
      page.texturedVertexCount = 0;
      page.indexCount = 0;
    }
    nextBufferPage_ = 0;
  }

  bool submit(const UiBatchSubmission &submission) noexcept override {
    try {
      if (!bgfx::isValid(submission.state.program) ||
          submission.indices.empty() ||
          submission.indices.size() > UiBatchRenderer::kMaximumIndices ||
          submission.state.uniformCount > submission.state.uniforms.size()) {
        return false;
      }

      const bool color = submission.format == UiBatchVertexFormat::Color;
      const auto vertexCount = color ? submission.colorVertices.size()
                                     : submission.texturedVertices.size();
      if (vertexCount == 0 || vertexCount > UiBatchRenderer::kMaximumVertices) {
        return false;
      }
      if (submission.state.texture.idx != bgfx::kInvalidHandle &&
          !bgfx::isValid(submission.state.texture)) {
        return false;
      }
      if (bgfx::isValid(submission.state.texture) &&
          !bgfx::isValid(submission.state.sampler)) {
        return false;
      }
      for (std::size_t index = 0; index < submission.state.uniformCount;
           ++index) {
        if (!bgfx::isValid(submission.state.uniforms[index].handle)) {
          return false;
        }
      }

      BufferPage *bufferPage =
          nextBufferPage(submission.format, vertexCount, submission.indices.size());
      if (bufferPage == nullptr) {
        return false;
      }
      const bgfx::Memory *vertexMemory =
          color ? bgfx::copy(submission.colorVertices.data(),
                             submission.colorVertices.size_bytes())
                : bgfx::copy(submission.texturedVertices.data(),
                             submission.texturedVertices.size_bytes());
      const bgfx::Memory *indexMemory =
          bgfx::copy(submission.indices.data(), submission.indices.size_bytes());
      if (vertexMemory == nullptr || indexMemory == nullptr) {
        return false;
      }
      const std::size_t vertexOffset =
          color ? bufferPage->colorVertexCount
                : bufferPage->texturedVertexCount;
      const std::size_t indexOffset = bufferPage->indexCount;
      const auto vertexBuffer = color ? bufferPage->colorVertexBuffer
                                      : bufferPage->texturedVertexBuffer;
      bgfx::update(vertexBuffer, static_cast<std::uint32_t>(vertexOffset),
                   vertexMemory);
      bgfx::update(bufferPage->indexBuffer,
                   static_cast<std::uint32_t>(indexOffset), indexMemory);
      bgfx::setVertexBuffer(0, vertexBuffer,
                            static_cast<std::uint32_t>(vertexOffset),
                            static_cast<std::uint32_t>(vertexCount));
      bgfx::setIndexBuffer(bufferPage->indexBuffer,
                           static_cast<std::uint32_t>(indexOffset),
                           static_cast<std::uint32_t>(submission.indices.size()));
      if (bgfx::isValid(submission.state.texture)) {
        bgfx::setTexture(0, submission.state.sampler, submission.state.texture,
                         submission.state.samplerFlags);
      }
      for (std::size_t index = 0; index < submission.state.uniformCount;
           ++index) {
        const auto &uniform = submission.state.uniforms[index];
        bgfx::setUniform(uniform.handle, uniform.value.data());
      }
      if (submission.state.transform) {
        bgfx::setTransform(submission.state.transform->data());
      }
      if (submission.state.scissor) {
        const auto &scissor = *submission.state.scissor;
        setScissorUI(scissor.x, scissor.y, scissor.width, scissor.height);
      } else {
        bgfx::setScissor();
      }
      bgfx::setState(submission.state.state);
      bgfx::submit(ui_view, submission.state.program);
      if (color) {
        bufferPage->colorVertexCount += vertexCount;
      } else {
        bufferPage->texturedVertexCount += vertexCount;
      }
      bufferPage->indexCount += submission.indices.size();
      return true;
    } catch (...) {
      return false;
    }
  }

  void shutdown() noexcept override {
    for (auto &page : bufferPages_) {
      if (bgfx::isValid(page.colorVertexBuffer)) {
        bgfx::destroy(page.colorVertexBuffer);
      }
      if (bgfx::isValid(page.texturedVertexBuffer)) {
        bgfx::destroy(page.texturedVertexBuffer);
      }
      if (bgfx::isValid(page.indexBuffer)) {
        bgfx::destroy(page.indexBuffer);
      }
    }
    bufferPages_.clear();
    nextBufferPage_ = 0;
  }

private:
  // bgfx executes a frame's dynamic-buffer updates before its draw list. Each
  // page therefore assigns every UI submission a non-overlapping range instead
  // of allocating one GPU buffer per texture/state transition.
  struct BufferPage {
    bgfx::DynamicVertexBufferHandle colorVertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle texturedVertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle indexBuffer = BGFX_INVALID_HANDLE;
    std::size_t colorVertexCount = 0;
    std::size_t texturedVertexCount = 0;
    std::size_t indexCount = 0;
  };

  BufferPage *nextBufferPage(UiBatchVertexFormat format,
                             std::size_t vertexCount,
                             std::size_t indexCount) noexcept {
    const auto canAppend =
        [format, vertexCount, indexCount](const BufferPage &page) noexcept {
      const std::size_t usedVertices =
          format == UiBatchVertexFormat::Color ? page.colorVertexCount
                                               : page.texturedVertexCount;
      return usedVertices <= UiBatchRenderer::kMaximumVertices - vertexCount &&
             page.indexCount <= UiBatchRenderer::kMaximumIndices - indexCount;
    };
    if (nextBufferPage_ == bufferPages_.size()) {
      try {
        bufferPages_.push_back({});
      } catch (...) {
        return nullptr;
      }
    }
    BufferPage *page = &bufferPages_[nextBufferPage_];
    if (!canAppend(*page)) {
      ++nextBufferPage_;
      return nextBufferPage(format, vertexCount, indexCount);
    }
    auto &vertexBuffer = format == UiBatchVertexFormat::Color
                             ? page->colorVertexBuffer
                             : page->texturedVertexBuffer;
    if (!bgfx::isValid(vertexBuffer)) {
      const auto &layout = format == UiBatchVertexFormat::Color
                               ? PosColorVertex::ms_decl
                               : PosTexCoord0Vertex::ms_decl;
      vertexBuffer = bgfx::createDynamicVertexBuffer(
          static_cast<std::uint32_t>(UiBatchRenderer::kMaximumVertices),
          layout);
    }
    if (!bgfx::isValid(page->indexBuffer)) {
      page->indexBuffer = bgfx::createDynamicIndexBuffer(
          static_cast<std::uint32_t>(UiBatchRenderer::kMaximumIndices));
    }
    return bgfx::isValid(vertexBuffer) && bgfx::isValid(page->indexBuffer)
               ? page
               : nullptr;
  }

  std::vector<BufferPage> bufferPages_;
  std::size_t nextBufferPage_ = 0;
};

} // namespace

UiBatchRenderer::UiBatchRenderer()
    : ownedBackend_(std::make_unique<BgfxUiBatchBackend>()),
      backend_(ownedBackend_.get()) {
  colorVertices_.reserve(4096);
  texturedVertices_.reserve(4096);
  indices_.reserve(6144);
}

UiBatchRenderer::UiBatchRenderer(UiBatchBackend &backend) noexcept
    : backend_(&backend) {
  colorVertices_.reserve(4096);
  texturedVertices_.reserve(4096);
  indices_.reserve(6144);
}

UiBatchRenderer::~UiBatchRenderer() { shutdown(); }

void UiBatchRenderer::beginFrame() noexcept {
  discard();
  if (backend_ != nullptr) {
    backend_->beginFrame();
  }
}

void UiBatchRenderer::begin() noexcept {
  discard();
  active_ = backend_ != nullptr;
}

bool UiBatchRenderer::appendColor(std::span<const PosColorVertex> vertices,
                                  std::span<const std::uint16_t> indices,
                                  const UiBatchState &state) {
  return append(UiBatchVertexFormat::Color, vertices, indices, state);
}

bool UiBatchRenderer::appendTextured(
    std::span<const PosTexCoord0Vertex> vertices,
    std::span<const std::uint16_t> indices, const UiBatchState &state) {
  return append(UiBatchVertexFormat::Textured, vertices, indices, state);
}

template <typename Vertex>
bool UiBatchRenderer::append(UiBatchVertexFormat format,
                             std::span<const Vertex> vertices,
                             std::span<const std::uint16_t> indices,
                             const UiBatchState &state) {
  if (!active_ || vertices.empty() || indices.empty() ||
      vertices.size() > kMaximumVertices || indices.size() > kMaximumIndices ||
      state.uniformCount > state.uniforms.size()) {
    return false;
  }
  if (std::ranges::any_of(indices, [&vertices](std::uint16_t index) {
        return index >= vertices.size();
      })) {
    return false;
  }
  const bool requiresFlush =
      !format_ || !state_ || *format_ != format || !sameState(*state_, state) ||
      (format == UiBatchVertexFormat::Color
           ? colorVertices_.size() + vertices.size() > kMaximumVertices
           : texturedVertices_.size() + vertices.size() > kMaximumVertices) ||
      indices_.size() + indices.size() > kMaximumIndices;
  const bool previousSubmissionFailed = requiresFlush && !flushCurrent();
  if (requiresFlush) {
    format_ = format;
    state_ = state;
  }

  const std::size_t base = format == UiBatchVertexFormat::Color
                               ? colorVertices_.size()
                               : texturedVertices_.size();
  if (base > kMaximumVertices - vertices.size()) {
    return false;
  }
  if constexpr (std::is_same_v<Vertex, PosColorVertex>) {
    colorVertices_.insert(colorVertices_.end(), vertices.begin(), vertices.end());
  } else {
    texturedVertices_.insert(texturedVertices_.end(), vertices.begin(),
                             vertices.end());
  }
  for (const auto index : indices) {
    indices_.push_back(static_cast<std::uint16_t>(base + index));
  }
  return !previousSubmissionFailed;
}

bool UiBatchRenderer::flush() noexcept {
  if (!active_) {
    discard();
    return false;
  }
  return flushCurrent();
}

void UiBatchRenderer::end() noexcept {
  flush();
  active_ = false;
}

void UiBatchRenderer::discard() noexcept { clearCurrent(); }

void UiBatchRenderer::shutdown() noexcept {
  discard();
  active_ = false;
  if (backend_ != nullptr) {
    backend_->shutdown();
  }
}

bool UiBatchRenderer::flushCurrent() noexcept {
  if (!format_ || !state_ || indices_.empty() || backend_ == nullptr) {
    clearCurrent();
    return true;
  }
  const UiBatchSubmission submission{
      .format = *format_,
      .colorVertices = *format_ == UiBatchVertexFormat::Color
                           ? std::span<const PosColorVertex>{colorVertices_}
                           : std::span<const PosColorVertex>{},
      .texturedVertices = *format_ == UiBatchVertexFormat::Textured
                              ? std::span<const PosTexCoord0Vertex>{texturedVertices_}
                              : std::span<const PosTexCoord0Vertex>{},
      .indices = indices_,
      .state = *state_};
  const bool submitted = backend_->submit(submission);
  if (!submitted) {
    static std::uint32_t failures = 0;
    ++failures;
    if (failures <= 3 || failures % 300 == 0) {
      SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                  "UiBatchRenderer: dynamic UI batch submission failed "
                  "(failures: %u).",
                  failures);
    }
  }
  clearCurrent();
  return submitted;
}

void UiBatchRenderer::clearCurrent() noexcept {
  colorVertices_.clear();
  texturedVertices_.clear();
  indices_.clear();
  format_.reset();
  state_.reset();
}

} // namespace rendering
