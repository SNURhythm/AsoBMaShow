#pragma once

#include <array>
#include <bgfx/bgfx.h>

struct AVFrame;
struct AVCodecContext;

namespace video {

// These handles reference decoder-owned surfaces; they never contain an upload.
// Destroy through bgfx so queued draws and the GPU retain the native resources.
struct HardwareVideoTextures {
  std::array<bgfx::TextureHandle, 2> planes{
      bgfx::TextureHandle{bgfx::kInvalidHandle},
      bgfx::TextureHandle{bgfx::kInvalidHandle}};
  std::array<std::array<float, 4>, 3> colorRows{};
};

inline void destroyHardwareVideoTextures(HardwareVideoTextures &textures) {
  for (auto &plane : textures.planes) {
    if (bgfx::isValid(plane)) bgfx::destroy(plane);
    plane = BGFX_INVALID_HANDLE;
  }
}

#if defined(__APPLE__)
bool hardwareVideoRenderingAvailable();
bool importHardwareVideoFrame(const AVFrame &frame, HardwareVideoTextures &out);
bool probeHardwareVideoFrame(const AVFrame &frame);
bool probeHardwareVideoDecoder(const AVCodecContext &context);
#else
inline bool hardwareVideoRenderingAvailable() { return false; }
inline bool importHardwareVideoFrame(const AVFrame &, HardwareVideoTextures &) { return false; }
inline bool probeHardwareVideoFrame(const AVFrame &) { return false; }
inline bool probeHardwareVideoDecoder(const AVCodecContext &) { return false; }
#endif

} // namespace video
