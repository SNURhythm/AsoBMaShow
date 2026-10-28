#include "skin/beatoraja/BgfxSkinTextureDevice.h"
#include "rendering/FrameSubmission.h"

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

void testCurrentFrameDrawAcceptance() {
  require(rendering::frameDrawsAccepted(), "an empty frame has no dropped draws");
  const auto capacity = bgfx::getCaps()->limits.maxDrawCalls;
  for (std::uint32_t draw = 0; draw < capacity; ++draw) bgfx::touch(0);
  require(rendering::frameDrawsAccepted(),
          "filling the draw capacity exactly is accepted before frame()");
  bgfx::touch(0);
  require(!rendering::frameDrawsAccepted(),
          "one draw beyond capacity is rejected before frame()");
  require(!rendering::frameDrawsAccepted(),
          "inspection does not consume the current frame's failure");
  bgfx::frame();
  require(rendering::frameDrawsAccepted(),
          "a fresh frame recovers from the previous frame's drops");
  bgfx::touch(0);
  require(rendering::frameDrawsAccepted(), "recovery draws are accepted");
  bgfx::frame();
  require(bgfx::getStats()->numDrawCallsPeak > capacity,
          "the lifetime peak still records the overloaded frame");
  require(rendering::frameDrawsAccepted(),
          "a historical overload does not poison later frames");

#if BGFX_CONFIG_MULTITHREADED
  auto *extra = bgfx::begin(true);
  require(extra != nullptr, "an extra encoder is available for the contract test");
  require(!rendering::frameDrawsAccepted(),
          "an active extra encoder is conservatively unsupported");
  extra->touch(0);
  bgfx::end(extra);
  require(!rendering::frameDrawsAccepted(),
          "a completed extra encoder remains unsupported for this frame");
  bgfx::frame();
  require(rendering::frameDrawsAccepted(),
          "extra encoder accounting resets at the next frame");
#endif
}

} // namespace

int main() {
  require(!rendering::frameDrawsAccepted(), "uninitialized bgfx cannot accept draws");
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init),
          "headless bgfx initializes for skin texture device tests");

  testValidRgbaUploadUsesOwnerThread();
  testMalformedAndOversizeImagesAreRejected();
  testChildThreadDoesNotOwnDevice();
  testCurrentFrameDrawAcceptance();

  bgfx::shutdown();
  require(!rendering::frameDrawsAccepted(), "shutdown bgfx cannot accept draws");
  return 0;
}
