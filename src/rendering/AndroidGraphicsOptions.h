#pragma once

#include <bgfx/bgfx.h>
#include <string_view>
#include <vector>

namespace rendering {

struct AndroidGraphicsOptions {
  bgfx::RendererType::Enum renderer = bgfx::RendererType::Count;
  // Bypass the swapchain MSAA resolve while investigating device-wide Android
  // corruption. This is a compatibility mitigation, not a confirmed diagnosis.
  int msaaSamples = 0;
};

enum class GraphicsArgumentResult { Ignored, Accepted, Invalid };

inline void configureAndroidRenderer(bgfx::Init &init,
                                     bgfx::RendererType::Enum renderer) {
  init.type = renderer;
  // The application owns fallback so each attempt receives the correct format,
  // and a forced-backend test cannot silently become a different-backend test.
  init.fallback = false;
  init.resolution.formatColor = renderer == bgfx::RendererType::Vulkan
                                    ? bgfx::TextureFormat::RGBA8
                                    : bgfx::TextureFormat::BGRA8;
}

inline GraphicsArgumentResult
parseAndroidGraphicsArgument(std::string_view argument,
                             AndroidGraphicsOptions &options) {
  constexpr std::string_view rendererPrefix = "--android-renderer=";
  constexpr std::string_view msaaPrefix = "--android-msaa=";
  if (argument.substr(0, rendererPrefix.size()) == rendererPrefix) {
    const auto value = argument.substr(rendererPrefix.size());
    if (value == "auto") {
      options.renderer = bgfx::RendererType::Count;
    } else if (value == "vulkan") {
      options.renderer = bgfx::RendererType::Vulkan;
    } else if (value == "gles") {
      options.renderer = bgfx::RendererType::OpenGLES;
    } else {
      return GraphicsArgumentResult::Invalid;
    }
    return GraphicsArgumentResult::Accepted;
  }
  if (argument.substr(0, msaaPrefix.size()) == msaaPrefix) {
    const auto value = argument.substr(msaaPrefix.size());
    if (value != "0" && value != "2") {
      return GraphicsArgumentResult::Invalid;
    }
    options.msaaSamples = value == "2" ? 2 : 0;
    return GraphicsArgumentResult::Accepted;
  }
  return GraphicsArgumentResult::Ignored;
}

inline uint32_t
androidGraphicsResetFlags(const AndroidGraphicsOptions &options) {
  return BGFX_RESET_VSYNC |
         (options.msaaSamples == 2 ? BGFX_RESET_MSAA_X2 : BGFX_RESET_NONE);
}

inline std::vector<bgfx::RendererType::Enum>
androidRendererCandidates(const AndroidGraphicsOptions &options,
                          bool emulator, int sdkVersion) {
  if (options.renderer != bgfx::RendererType::Count) {
    // A forced backend must fail visibly instead of invalidating an A/B test
    // by silently selecting a different renderer.
    return {options.renderer};
  }
  if (emulator && sdkVersion >= 33) {
    return {bgfx::RendererType::OpenGLES};
  }
  return {bgfx::RendererType::Vulkan, bgfx::RendererType::OpenGLES};
}

} // namespace rendering
