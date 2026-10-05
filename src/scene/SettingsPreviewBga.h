#pragma once

#include "../audio/Jukebox.h"
#include "../RAII.h"
#include <cmath>

namespace settings_scene {
inline constexpr int kPreviewBgaWidth = 960;
inline constexpr int kPreviewBgaHeight = 540;

// A quiet, high-contrast still life of colored light and concentric arcs.
// Generate it once; all sizing, compositing and post-processing remain in BGA.
inline std::vector<std::uint8_t> previewBgaPixels() {
  std::vector<std::uint8_t> pixels(kPreviewBgaWidth * kPreviewBgaHeight * 4);
  for (int y = 0; y < kPreviewBgaHeight; ++y) {
    for (int x = 0; x < kPreviewBgaWidth; ++x) {
      const float u = float(x) / kPreviewBgaWidth;
      const float v = float(y) / kPreviewBgaHeight;
      const float cool = std::exp(-9.0F * ((u - 0.24F) * (u - 0.24F) +
                                          (v - 0.36F) * (v - 0.36F)));
      const float warm = std::exp(-16.0F * ((u - 0.76F) * (u - 0.76F) +
                                           (v - 0.60F) * (v - 0.60F)));
      const float radius = std::hypot((u - 0.5F) * 1.77778F, v - 0.5F);
      const float arc = std::pow(std::max(0.0F, std::cos(radius * 38.0F)), 32.0F) * 18.0F;
      const std::array channels{25.0F + 20.0F * cool + 110.0F * warm + arc,
                                32.0F + 105.0F * cool + 56.0F * warm + arc,
                                50.0F + 130.0F * cool + 35.0F * warm + arc};
      const auto offset = (y * kPreviewBgaWidth + x) * 4;
      for (int channel = 0; channel < 3; ++channel)
        pixels[offset + channel] = static_cast<std::uint8_t>(std::clamp(channels[channel], 0.0F, 255.0F));
      pixels[offset + 3] = 255;
    }
  }
  return pixels;
}

class PreviewBga final : public IGameplayBgaSubmitter {
public:
  PreviewBga(Jukebox &jukebox, const AppSettings &settings) : jukebox(jukebox), settings(settings) {}

  PreparedGameplayBgaFrame prepareVisualFrameAt(std::uint64_t, std::int64_t,
                                                const GameplayBgaMissState &) override {
    jukebox.setBgaDisplayMode(settings.bgaDisplayMode);
    jukebox.setEmbeddedBgaBrightnessPercent(settings.bgaBrightnessPercent);
    if (!settings.bgaEnabled) return jukebox.prepareImageFrame({});
    if (!image) image = makeImage();
    return jukebox.prepareImageFrame(image);
  }
  BgaPreflightResult preflight(const PreparedGameplayBgaFrame &frame,
                               std::span<const BgaDrawTarget> targets) override {
    return jukebox.preflight(frame, targets);
  }
  void commitPrepared(const PreparedGameplayBgaFrame &frame) noexcept override {
    jukebox.commitPrepared(frame);
  }
  void submitPrepared(const PreparedGameplayBgaFrame &frame, const BgaDrawTarget &target) noexcept override {
    jukebox.submitPrepared(frame, target);
  }
  void finalizePrepared(const PreparedGameplayBgaFrame &frame) noexcept override {
    jukebox.finalizePrepared(frame);
  }
  void submitFullscreen(const PreparedGameplayBgaFrame &frame) noexcept override {
    jukebox.submitFullscreen(frame);
  }

private:
  static std::shared_ptr<ImageData> makeImage() {
    const auto pixels = previewBgaPixels();
    ImageData image{.width = kPreviewBgaWidth, .height = kPreviewBgaHeight, .channels = 4};
    image.texture = bgfx::createTexture2D(kPreviewBgaWidth, kPreviewBgaHeight, false, 1,
        bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(pixels.data(), static_cast<std::uint32_t>(pixels.size())));
    const auto guard = makeScopeExit([&image] {
      if (bgfx::isValid(image.texture)) bgfx::destroy(image.texture);
    });
    if (!bgfx::isValid(image.texture)) return {};
    return AdoptImageTextureToSharedOwner(image);
  }
  Jukebox &jukebox;
  const AppSettings &settings;
  std::shared_ptr<ImageData> image;
};
} // namespace settings_scene
