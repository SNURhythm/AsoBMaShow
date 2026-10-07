#include "rendering/ShaderManager.h"
#include "rendering/UiBatchRenderer.h"
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
  const std::uint32_t green = 0xff00ff00U;
  const auto greenTexture = bgfx::createTexture2D(
      1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0, bgfx::copy(&green, 4));
  bool passed = bgfx::isValid(output) && bgfx::isValid(readback) &&
                bgfx::isValid(framebuffer) && bgfx::isValid(texture) &&
                bgfx::isValid(greenTexture);
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
    const auto opacityProgram =
        rendering::ShaderManager::getInstance().getProgram("vs_skin_quad.bin",
                                                          "fs_skin_quad.bin");
    const auto sampling =
        rendering::UniformCache::getInstance().getVec4("u_skinSampling");
    constexpr std::array opacities{0.5f, 0.5f, 0.25f, 1.0f, 0.5f,
                                   0.5f, 0.5f, 0.75f, 0.5f};
    constexpr std::array opacityQuads{1, 1, 1, 1, 1, 2, 2, 3, 1};
    rendering::UiBatchRenderer renderer;
    for (int frame = 0; frame < 46 + static_cast<int>(opacities.size()); ++frame) {
      renderer.beginFrame();
      renderer.begin();
      // Each tile is one draw slot. Scene changes alter both the batch sizes
      // and vertex formats. The last frames reuse the original geometry while
      // changing scissor, index contents and texture state independently.
      const int scene = frame < 40 ? frame : 0;
      const bool opacityFrame = frame >= 46;
      const float opacity = opacityFrame ? opacities[frame - 46] : 1.0f;
      for (int tile = 0; tile < 64; ++tile) {
        const int quads = opacityFrame ? opacityQuads[frame - 46]
                                      : 1 + ((scene * 103 + tile * 37) % 311);
        const float x = (tile % 8) * 16;
        const float y = (tile / 8) * 16;
        std::vector<rendering::PosColorVertex> colorVertices;
        std::vector<rendering::PosTexCoord0Vertex> textVertices;
        std::vector<std::uint16_t> indices;
        // Overlapping opaque quads vary storage demand without changing the
        // expected image: red color tiles alternate with white texture tiles.
        for (int quad = 0; quad < quads; ++quad) {
          // Opacity frames use adjacent quads, so repeated alpha blending does
          // not hide stale opacity data as the stream grows and shrinks.
          const float left = opacityFrame ? x + 16.0f * quad / quads : x;
          const float right = opacityFrame ? x + 16.0f * (quad + 1) / quads : x + 16;
          colorVertices.insert(colorVertices.end(),
              {{left, y, 0, 0xff0000ffU}, {right, y, 0, 0xff0000ffU},
               {right, y + 16, 0, 0xff0000ffU}, {left, y + 16, 0, 0xff0000ffU}});
          textVertices.insert(textVertices.end(),
              {{left, y, 0, 0, 0}, {right, y, 0, 1, 0},
               {right, y + 16, 0, 1, 1}, {left, y + 16, 0, 0, 1}});
          const auto quadIndices = frame == 42
                                       ? std::array{0, 1, 2, 0, 1, 2}
                                       : std::array{0, 1, 2, 0, 2, 3};
          for (const int index : quadIndices) {
            indices.push_back(static_cast<std::uint16_t>(quad * 4 + index));
          }
        }
        rendering::UiBatchState state{
            .program = colorProgram,
            .state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A};
        if (frame == 41) {
          state.scissor = rendering::UiBatchScissor{
              .x = static_cast<int>(x), .y = static_cast<int>(y),
              .width = 8, .height = 16};
        }
        bool appended;
        if ((tile + scene) % 2 != 0) {
          state.program = textProgram;
          state.texture = frame == 43 || frame == 44 ? greenTexture : texture;
          state.sampler = sampler;
          if (opacityFrame) {
            state.state |= BGFX_STATE_BLEND_ALPHA;
            state.textureOpacity = opacity;
            if (opacity < 1.0f) {
              // Match RenderContext's actual faded-text shader and stream.
              state.program = opacityProgram;
              state.uniforms[0] = {.handle = sampling, .value = {0, 0, 0, 0}};
              state.uniformCount = 1;
            }
          }
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
            if (frame == 42 && x == y) continue; // Triangle edge coverage varies.
            const auto pixel = ((tile / 8 * 16 + y) * 128 + tile % 8 * 16 + x) * 4;
            const bool clipped = (frame == 41 && x >= 8) ||
                                 (frame == 42 && x < y);
            const bool greenTextured = textured && (frame == 43 || frame == 44);
            const int expectedBlue = !clipped && textured && !greenTextured ? 255 : 0;
            const int expectedGreen = !clipped && textured ? 255 : 0;
            const int expectedRed = !clipped && !greenTextured ? 255 : 0;
            if (opacityFrame && textured) {
              const int expected = static_cast<int>(std::lround(255.0f * opacity));
              for (int channel = 0; channel < 3; ++channel) {
                corrupted |= std::abs(static_cast<int>(pixels[pixel + channel]) -
                                      expected) > 1;
              }
            } else {
              corrupted |= pixels[pixel] != expectedBlue ||
                           pixels[pixel + 1] != expectedGreen ||
                           pixels[pixel + 2] != expectedRed;
            }
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
  if (bgfx::isValid(greenTexture)) bgfx::destroy(greenTexture);
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
