#include "rendering/ShaderManager.h"
#include "rendering/UiBatchRenderer.h"
#include "rendering/UniformCache.h"

#include <bx/math.h>

#include <array>
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

bool testMixedUiBatchesSurviveGrowthAndSceneChanges() {
  const auto output = bgfx::createTexture2D(
      128, 128, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(
      128, 128, false, 1, bgfx::TextureFormat::BGRA8,
      BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  const auto framebuffer = bgfx::isValid(output)
                               ? bgfx::createFrameBuffer(1, &output, false)
                               : bgfx::FrameBufferHandle{bgfx::kInvalidHandle};
  const std::uint32_t white = 0xffffffffU;
  const auto texture = bgfx::createTexture2D(
      1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(&white, 4));
  bool passed = bgfx::isValid(output) && bgfx::isValid(readback) &&
                bgfx::isValid(framebuffer) && bgfx::isValid(texture);
  if (!passed) std::cerr << "FAIL: Metal readback resources are unavailable\n";

  if (passed) {
    float ortho[16];
    bx::mtxOrtho(ortho, 0, 128, 128, 0, 0, 100, 0,
                 bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewFrameBuffer(rendering::ui_view, framebuffer);
    bgfx::setViewRect(rendering::ui_view, 0, 0, 128, 128);
    bgfx::setViewTransform(rendering::ui_view, nullptr, ortho);
    bgfx::setViewMode(rendering::ui_view, bgfx::ViewMode::Sequential);
    bgfx::setViewClear(rendering::ui_view, BGFX_CLEAR_COLOR, 0x000000ffU);
    const auto sampler =
        rendering::UniformCache::getInstance().getSampler("s_texColor");
    const auto colorProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
    const auto textProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
    rendering::UiBatchRenderer renderer;
    for (int frame = 0; frame < 44; ++frame) {
      renderer.beginFrame();
      renderer.begin();
      // Each tile is one draw slot. Scene changes alter both the batch sizes
      // and vertex formats. The last frames return to the original scene.
      const int scene = frame < 40 ? frame : 0;
      for (int tile = 0; tile < 64; ++tile) {
        const int quads = 1 + ((scene * 103 + tile * 37) % 311);
        const float x = (tile % 8) * 16;
        const float y = (tile / 8) * 16;
        std::vector<rendering::PosColorVertex> colorVertices;
        std::vector<rendering::PosTexCoord0Vertex> textVertices;
        std::vector<std::uint16_t> indices;
        // Overlapping opaque quads vary storage demand without changing the
        // expected image: red color tiles alternate with white texture tiles.
        for (int quad = 0; quad < quads; ++quad) {
          colorVertices.insert(colorVertices.end(),
              {{x, y, 0, 0xff0000ffU}, {x + 16, y, 0, 0xff0000ffU},
               {x + 16, y + 16, 0, 0xff0000ffU}, {x, y + 16, 0, 0xff0000ffU}});
          textVertices.insert(textVertices.end(),
              {{x, y, 0, 0, 0}, {x + 16, y, 0, 1, 0},
               {x + 16, y + 16, 0, 1, 1}, {x, y + 16, 0, 0, 1}});
          for (const int index : {0, 1, 2, 0, 2, 3}) {
            indices.push_back(static_cast<std::uint16_t>(quad * 4 + index));
          }
        }
        rendering::UiBatchState state{
            .program = colorProgram,
            .state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A};
        bool appended;
        if ((tile + scene) % 2 != 0) {
          state.program = textProgram;
          state.texture = texture;
          state.sampler = sampler;
          appended = renderer.appendTextured(textVertices, indices, state);
        } else {
          appended = renderer.appendColor(colorVertices, indices, state);
        }
        const bool flushed = renderer.flush();
        passed = appended && flushed && passed;
      }
      renderer.end();
      bgfx::blit(rendering::readback_view, readback, 0, 0, output);
      auto currentFrame = bgfx::frame();
      std::vector<std::uint8_t> pixels(128 * 128 * 4);
      const auto readyFrame = bgfx::readTexture(readback, pixels.data());
      for (int guard = 0; currentFrame < readyFrame && guard < 16; ++guard) {
        currentFrame = bgfx::frame();
      }
      if (readyFrame == std::numeric_limits<std::uint32_t>::max() ||
          currentFrame < readyFrame) {
        std::cerr << "FAIL: Metal readback did not complete\n";
        passed = false;
        break;
      }
      int corruptedTiles = 0;
      for (int tile = 0; tile < 64; ++tile) {
        const bool textured = (tile + scene) % 2 != 0;
        bool corrupted = false;
        for (int y = 2; y < 14; ++y) {
          for (int x = 2; x < 14; ++x) {
            const auto pixel = ((tile / 8 * 16 + y) * 128 + tile % 8 * 16 + x) * 4;
            const auto expected = textured ? 255 : 0;
            corrupted |= pixels[pixel] != expected ||
                         pixels[pixel + 1] != expected ||
                         pixels[pixel + 2] != 255;
          }
        }
        if (corrupted) ++corruptedTiles;
      }
      if (corruptedTiles != 0) {
        std::cerr << "FAIL: frame " << frame << " has " << corruptedTiles
                  << " corrupted UI tiles\n";
        passed = false;
      }
    }
  }

  bgfx::setViewFrameBuffer(rendering::ui_view, BGFX_INVALID_HANDLE);
  if (bgfx::isValid(framebuffer)) bgfx::destroy(framebuffer);
  if (bgfx::isValid(output)) bgfx::destroy(output);
  if (bgfx::isValid(readback)) bgfx::destroy(readback);
  if (bgfx::isValid(texture)) bgfx::destroy(texture);
  return passed;
}

} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Metal;
  init.fallback = false;
  init.resolution.width = 0;
  init.resolution.height = 0;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless Metal initialization failed\n";
    return 1;
  }
  const bool passed = testMixedUiBatchesSurviveGrowthAndSceneChanges();
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::frame();
  bgfx::frame();
  bgfx::shutdown();
  if (!passed) return 1;
  std::cout << "UI batch real-Metal tests passed\n";
  return 0;
}
