#include "skin/beatoraja/BgfxSkinTextureDevice.h"

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

image_decode::DecodedImageData makeImage(int width, int height) {
  return {.width = width,
          .height = height,
          .rgba = std::make_shared<std::vector<unsigned char>>(
              static_cast<std::size_t>(width) *
                  static_cast<std::size_t>(height) * 4U,
              0xff)};
}

void testValidRgbaUploadUsesOwnerThread() {
  skin::BgfxSkinTextureDevice device;
  require(device.ownsCurrentThread(),
          "texture device owns the thread on which it was constructed");

  const auto texture = device.create(makeImage(2, 3));
  require(bgfx::isValid(texture),
          "valid RGBA pixels create a bgfx texture on the owner thread");
  device.destroy(texture);
}

void testMalformedAndOversizeImagesAreRejected() {
  skin::BgfxSkinTextureDevice device;
  require(!bgfx::isValid(device.create({})),
          "zero-dimensional images without RGBA storage are rejected");
  image_decode::DecodedImageData nullRgba{.width = 1, .height = 1};
  require(!bgfx::isValid(device.create(nullRgba)),
          "missing RGBA storage is rejected before upload");

  auto malformed = makeImage(2, 2);
  malformed.rgba->pop_back();
  require(!bgfx::isValid(device.create(malformed)),
          "RGBA byte-count mismatch is rejected before upload");

  const int maximum = device.maximumTextureDimension();
  require(maximum > 0 && maximum <= skin::SkinResourcePolicy::maximumDimension,
          "active bgfx texture limit is clamped to the skin resource policy");
  const int unrestrictedMaximum = device.maximumTextureDimension(
      skin::SkinSafetyPolicy{skin::SkinSafetyLevel::Unrestricted});
  require(unrestrictedMaximum >= maximum,
          "unrestricted texture limit never tightens the standard limit");
  require(unrestrictedMaximum ==
              static_cast<int>(std::min<std::uint32_t>(
                  bgfx::getCaps()->limits.maxTextureSize,
                  std::numeric_limits<std::uint16_t>::max())),
          "unrestricted texture limit follows the actual bgfx device limit");
  image_decode::DecodedImageData oversize{
      .width = maximum + 1, .height = 1,
      .rgba = std::make_shared<std::vector<unsigned char>>(4U)};
  require(!bgfx::isValid(device.create(oversize)),
          "images beyond the device or skin dimension cap are rejected");
}

void testChildThreadDoesNotOwnDevice() {
  skin::BgfxSkinTextureDevice device;
  bool childOwnsDevice = true;
  std::thread child([&] { childOwnsDevice = device.ownsCurrentThread(); });
  child.join();
  require(!childOwnsDevice,
          "a child thread does not own a render-thread texture device");
}

} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init),
          "headless bgfx initializes for skin texture device tests");

  testValidRgbaUploadUsesOwnerThread();
  testMalformedAndOversizeImagesAreRejected();
  testChildThreadDoesNotOwnDevice();

  bgfx::shutdown();
  return 0;
}
