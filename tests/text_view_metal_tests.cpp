#include "view/TextView.h"
#include "view/IconText.h"
#include "rendering/UniformCache.h"

#include <bx/math.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace rendering {
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = 128;
int window_height = 128;
int render_width = 128;
int render_height = 128;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = 128;
int ui_view_height = 128;
} // namespace rendering

namespace {

bool testIconEdgesDoNotWrap() {
  constexpr int size = 256;
  const auto output = bgfx::createTexture2D(
      size, size, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(
      size, size, false, 1, bgfx::TextureFormat::BGRA8,
      BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  bool passed = bgfx::isValid(output) && bgfx::isValid(readback) &&
                bgfx::isValid(framebuffer);
  if (!passed) return false;

  // A larger drawable exposes filtering across the edges of tightly cropped
  // icon rasters, as fractional UI/device scaling does on Retina displays.
  float ortho[16];
  bx::mtxOrtho(ortho, 0, 64, 64, 0, 0, 100, 0,
               bgfx::getCaps()->homogeneousDepth);
  bgfx::setViewFrameBuffer(rendering::ui_view, framebuffer);
  bgfx::setViewRect(rendering::ui_view, 0, 0, size, size);
  bgfx::setViewTransform(rendering::ui_view, nullptr, ortho);
  bgfx::setViewMode(rendering::ui_view, bgfx::ViewMode::Sequential);
  bgfx::setViewClear(rendering::ui_view, BGFX_CLEAR_COLOR, 0x000000ffU);
  {
    rendering::UiBatchRenderer renderer;
    RenderContext context(renderer);
    TextView icon(ui_icons::kFontAwesomeSolidPath, 16);
    for (const auto codepoint : {ui_icons::kExpand, ui_icons::kCollapse,
                                 ui_icons::kMusic, ui_icons::kRecords,
                                 ui_icons::kDownload}) {
      icon.setText(ui_icons::textForCodepoint(codepoint));
      icon.setSize(icon.textureWidth(), icon.textureHeight());
      const float width = icon.textureWidth();
      const float height = icon.textureHeight();
      const std::array vertices = {
          rendering::PosTexCoord0Vertex{0, 0, 0, 0, 0},
          rendering::PosTexCoord0Vertex{width, 0, 0, 1, 0},
          rendering::PosTexCoord0Vertex{width, height, 0, 1, 1},
          rendering::PosTexCoord0Vertex{0, height, 0, 0, 1}};
      constexpr std::array<std::uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
      std::array<std::vector<std::uint8_t>, 2> images;
      for (int reference = 0; reference < 2; ++reference) {
        renderer.beginFrame();
        {
          RenderContext::UiBatchScope batch(context);
          if (reference == 0) {
            icon.render(context);
          } else {
            // Reference the same glyph raster with non-repeating, linear
            // sampling. This preserves its edge pixels and antialiasing.
            auto state = context.makeUiBatchState(
                rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT),
                BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
            state.texture = icon.textureHandle();
            state.sampler = rendering::UniformCache::getInstance().getSampler("s_texColor");
            state.samplerFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
            context.appendUiTextured(vertices, indices, state);
          }
        }
        bgfx::touch(rendering::ui_view);
        bgfx::blit(rendering::readback_view, readback, 0, 0, output);
        auto frame = bgfx::frame();
        images[reference].resize(size * size * 4);
        const auto ready = bgfx::readTexture(readback, images[reference].data());
        for (int guard = 0; frame < ready && guard < 16; ++guard) frame = bgfx::frame();
        if (ready == std::numeric_limits<std::uint32_t>::max() || frame < ready) {
          passed = false;
          break;
        }
      }
      if (!passed) break;
      int artifacts = 0;
      for (std::size_t pixel = 0; pixel < images[0].size(); pixel += 4) {
        if (std::abs(int(images[0][pixel]) - int(images[1][pixel])) > 1) ++artifacts;
      }
      if (artifacts != 0) {
        std::cerr << "FAIL: icon " << codepoint << " has " << artifacts
                  << " pixels contaminated by texture-edge wrapping\n";
        passed = false;
      }
    }
  }
  bgfx::setViewFrameBuffer(rendering::ui_view, BGFX_INVALID_HANDLE);
  bgfx::destroy(framebuffer);
  bgfx::destroy(readback);
  bgfx::destroy(output);
  return passed;
}

} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Metal;
  init.fallback = false;
  init.resolution.width = 0;
  init.resolution.height = 0;
  if (!bgfx::init(init)) return 1;
  const bool passed = testIconEdgesDoNotWrap();
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::frame();
  bgfx::frame();
  bgfx::shutdown();
  return passed ? 0 : 1;
}
