#pragma once

#include "SkinResourceCatalog.h"

#include <thread>

namespace skin {

// The catalog owns exact-once texture lifetime; this adapter owns only the
// render-thread boundary.
class BgfxSkinTextureDevice final : public SkinTextureDevice {
public:
  BgfxSkinTextureDevice();

  bgfx::TextureHandle create(const image_decode::DecodedImageData &) override;
  void destroy(bgfx::TextureHandle) noexcept override;
  [[nodiscard]] bool ownsCurrentThread() const noexcept override;
  [[nodiscard]] int maximumTextureDimension() const noexcept override;

private:
  std::thread::id owner_;
};

} // namespace skin
