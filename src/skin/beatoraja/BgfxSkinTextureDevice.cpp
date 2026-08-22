#include "BgfxSkinTextureDevice.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>

namespace skin {

namespace {

[[nodiscard]] int activeMaximumTextureDimension(
    SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{}) noexcept {
  const auto *caps = bgfx::getCaps();
  if (caps == nullptr || caps->limits.maxTextureSize == 0) {
    return 0;
  }
  const std::uint32_t representableMaximum = std::min<std::uint32_t>(
      caps->limits.maxTextureSize,
      std::numeric_limits<std::uint16_t>::max());
  const auto deviceMaximum = safetyPolicy.enforces(
      SkinSafetyGuard::ResourceAllocationLimit)
      ? std::min<std::uint32_t>(
            representableMaximum,
            static_cast<std::uint32_t>(SkinResourcePolicy::maximumDimension))
      : representableMaximum;
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
  return create(image, SkinSafetyPolicy{});
}

int BgfxSkinTextureDevice::maximumTextureDimension(
    SkinSafetyPolicy safetyPolicy) const noexcept {
  return activeMaximumTextureDimension(safetyPolicy);
}

bgfx::TextureHandle BgfxSkinTextureDevice::create(
    const image_decode::DecodedImageData &image,
    SkinSafetyPolicy safetyPolicy) {
  if (!ownsCurrentThread()) {
    return BGFX_INVALID_HANDLE;
  }

  const int maximumDimension = activeMaximumTextureDimension(safetyPolicy);
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

bool BgfxSkinTextureDevice::update(
    bgfx::TextureHandle texture,
    const image_decode::DecodedImageData &image) {
  if (!ownsCurrentThread() || !bgfx::isValid(texture) || image.width <= 0 ||
      image.height <= 0 ||
      image.width > std::numeric_limits<std::uint16_t>::max() ||
      image.height > std::numeric_limits<std::uint16_t>::max() ||
      image.rgba == nullptr) {
    return false;
  }
  const auto byteCount = static_cast<std::uint64_t>(image.width) *
                         static_cast<std::uint64_t>(image.height) * 4U;
  if (byteCount > std::numeric_limits<std::uint32_t>::max() ||
      image.rgba->size() != static_cast<std::size_t>(byteCount)) {
    return false;
  }
  const bgfx::Memory *memory = bgfx::copy(
      image.rgba->data(), static_cast<std::uint32_t>(byteCount));
  if (memory == nullptr) {
    return false;
  }
  bgfx::updateTexture2D(texture, 0, 0, 0, 0,
                        static_cast<std::uint16_t>(image.width),
                        static_cast<std::uint16_t>(image.height), memory);
  return true;
}

} // namespace skin
