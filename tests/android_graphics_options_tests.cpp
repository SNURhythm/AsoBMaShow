#include "rendering/AndroidGraphicsOptions.h"

#include <cstdlib>
#include <iostream>

static void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main() {
  using namespace rendering;
  using Renderer = bgfx::RendererType;
  bgfx::Init init;
  init.resolution.reset = BGFX_RESET_VSYNC;
  configureAndroidRenderer(init, Renderer::Vulkan);
  require(init.type == Renderer::Vulkan && !init.fallback &&
              init.resolution.formatColor == bgfx::TextureFormat::RGBA8,
          "A Vulkan attempt must not let bgfx silently initialize GLES");
  configureAndroidRenderer(init, Renderer::OpenGLES);
  require(init.type == Renderer::OpenGLES && !init.fallback &&
              init.resolution.formatColor == bgfx::TextureFormat::BGRA8 &&
              init.resolution.reset == BGFX_RESET_VSYNC,
          "GLES fallback must use its own format without changing reset flags");
  AndroidGraphicsOptions options;
  require(androidGraphicsResetFlags(options) == BGFX_RESET_VSYNC,
          "Default Android startup must bypass MSAA and retain vsync");
  require(androidRendererCandidates(options, false, 33) ==
              std::vector<Renderer::Enum>{Renderer::Vulkan, Renderer::OpenGLES},
          "Physical devices must retain Vulkan-first automatic fallback");
  require(androidRendererCandidates(options, true, 33) ==
              std::vector<Renderer::Enum>{Renderer::OpenGLES},
          "Automatic mode must avoid the newer emulator Vulkan stub");
  require(androidRendererCandidates(options, true, 29) ==
              std::vector<Renderer::Enum>{Renderer::Vulkan, Renderer::OpenGLES},
          "Older emulator automatic selection must remain available");

  require(parseAndroidGraphicsArgument("--android-renderer=gles", options) ==
              GraphicsArgumentResult::Accepted,
          "GLES override must be accepted");
  require(androidRendererCandidates(options, false, 33) ==
              std::vector<Renderer::Enum>{Renderer::OpenGLES},
          "Forced GLES must never attempt Vulkan");
  require(parseAndroidGraphicsArgument("--android-renderer=vulkan", options) ==
              GraphicsArgumentResult::Accepted,
          "Vulkan override must be accepted");
  require(androidRendererCandidates(options, true, 33) ==
              std::vector<Renderer::Enum>{Renderer::Vulkan},
          "Forced Vulkan must not silently fall back, even on an emulator");
  require(parseAndroidGraphicsArgument("--android-msaa=2", options) ==
              GraphicsArgumentResult::Accepted,
          "MSAA comparison override must be accepted");
  require(androidGraphicsResetFlags(options) ==
              (BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X2),
          "MSAA override must retain vsync");

  for (const auto argument : {"--android-renderer=", "--android-renderer=metal",
                              "--android-msaa=-1", "--android-msaa=4",
                              "--android-msaa=2junk"}) {
    require(parseAndroidGraphicsArgument(argument, options) ==
                GraphicsArgumentResult::Invalid,
            "Invalid options must be rejected");
    require(options.renderer == Renderer::Vulkan && options.msaaSamples == 2,
            "Invalid options must not partially change prior valid options");
  }
  require(parseAndroidGraphicsArgument("unrelated", options) ==
              GraphicsArgumentResult::Ignored,
          "Unrelated startup arguments must be ignored");
  require(parseAndroidGraphicsArgument("--android-msaa=0", options) ==
              GraphicsArgumentResult::Accepted &&
              androidGraphicsResetFlags(options) == BGFX_RESET_VSYNC,
          "Explicit MSAA off must remove all sample flags");
  require(parseAndroidGraphicsArgument("--android-renderer=auto", options) ==
              GraphicsArgumentResult::Accepted &&
              androidRendererCandidates(options, false, 33).size() == 2,
          "Auto must restore fallback after an override");
  std::cout << "Android graphics options passed\n";
}
