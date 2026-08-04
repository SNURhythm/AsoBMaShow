#include "BgfxSkinTextureDevice.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>

namespace skin {

namespace {

[[nodiscard]] int activeMaximumTextureDimension() noexcept {
  const auto *caps = bgfx::getCaps();
  if (caps == nullptr || caps->limits.maxTextureSize == 0) {
    return 0;
  }
  const auto deviceMaximum = std::min<std::uint32_t>(
      caps->limits.maxTextureSize,
      static_cast<std::uint32_t>(SkinResourcePolicy::maximumDimension));
  return static_cast<int>(deviceMaximum);
}

} // namespace

BgfxSkinTextureDevice::BgfxSkinTextureDevice()
    : owner_(std::this_thread::get_id()) {}

bool BgfxSkinTextureDevice::ownsCurrentThread() const noexcept {
  return std::this_thread::get_id() == owner_;
}

int BgfxSkinTextureDevice::maximumTextureDimension() const noexcept {
  return activeMaximumTextureDimension();
}

bgfx::TextureHandle BgfxSkinTextureDevice::create(
    const image_decode::DecodedImageData &image) {
  if (!ownsCurrentThread()) {
    return BGFX_INVALID_HANDLE;
  }

  const int maximumDimension = activeMaximumTextureDimension();
  if (maximumDimension <= 0 || image.width <= 0 || image.height <= 0 ||
      image.width > maximumDimension || image.height > maximumDimension ||
      image.width > std::numeric_limits<std::uint16_t>::max() ||
      image.height > std::numeric_limits<std::uint16_t>::max() ||
      image.rgba == nullptr) {
    return BGFX_INVALID_HANDLE;
  }

  const auto pixelCount = static_cast<std::uint64_t>(image.width) *
                          static_cast<std::uint64_t>(image.height);
  const auto byteCount = pixelCount * 4U;
  if (byteCount > std::numeric_limits<std::size_t>::max() ||
      byteCount > std::numeric_limits<std::uint32_t>::max() ||
      image.rgba->size() != static_cast<std::size_t>(byteCount)) {
    return BGFX_INVALID_HANDLE;
  }

  const bgfx::Memory *memory = bgfx::copy(
      image.rgba->data(), static_cast<std::uint32_t>(byteCount));
  if (memory == nullptr) {
    return BGFX_INVALID_HANDLE;
  }
  const auto texture = bgfx::createTexture2D(
      static_cast<std::uint16_t>(image.width),
      static_cast<std::uint16_t>(image.height), false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, memory);
  return texture;
}

void BgfxSkinTextureDevice::destroy(bgfx::TextureHandle texture) noexcept {
  if (!bgfx::isValid(texture)) {
    return;
  }
  if (!ownsCurrentThread()) {
    std::terminate();
  }
  bgfx::destroy(texture);
}

} // namespace skin
