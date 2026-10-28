#include "view/TextView.h"
#include "view/IconText.h"
#include "view/AppTheme.h"
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

bool testGlyphTexturesAreSharedAndReused() {
  struct Capture final : rendering::UiBatchBackend {
    std::vector<uint16_t> textures;
    std::size_t vertices = 0;
    bool submit(const rendering::UiBatchSubmission &draw) noexcept override {
      textures.push_back(draw.state.texture.idx);
      vertices += draw.texturedVertices.size();
      return true;
    }
  } capture;
  rendering::UiBatchRenderer renderer(capture);
  RenderContext context(renderer);
  TextView first(app_theme::font, 24), second(app_theme::font, 24);
  first.setGlyphAtlasEnabled(true);
  second.setGlyphAtlasEnabled(true);
  first.setSize(290, 100);
  second.setSize(290, 100);
  const auto draw = [&](TextView &view) {
    capture.textures.clear();
    capture.vertices = 0;
    renderer.beginFrame();
    { RenderContext::UiBatchScope batch(context); view.render(context); }
  };
  first.setText("0123456789");
  first.setVisible(false);
  draw(first);
  if (!capture.textures.empty() || bgfx::isValid(first.textureHandle())) return false;
  first.setVisible(true);
  draw(first);
  const auto pages = capture.textures;
  if (pages.size() != 1 || pages.front() == bgfx::kInvalidHandle ||
      capture.vertices != 40 || bgfx::isValid(first.textureHandle())) return false;
  first.setText("9876543210");
  first.setColor({30, 120, 220, 80});
  draw(first);
  if (capture.textures != pages || bgfx::isValid(first.textureHandle())) return false;
  second.setText("1122334455");
  draw(second);
  if (capture.textures != pages || bgfx::isValid(second.textureHandle())) return false;
  // Unsupported text can fall back and then return to the same warmed atlas.
  second.setWrap(true);
  draw(second);
  if (!bgfx::isValid(second.textureHandle())) return false;
  second.setWrap(false);
  draw(second);
  if (capture.textures != pages || bgfx::isValid(second.textureHandle())) return false;
  second.setText("");
  draw(second);
  return capture.textures.empty();
}

bool testChangingTextUsesGlyphAtlas() {
  constexpr int size = 512;
  const auto output = bgfx::createTexture2D(
      size, size, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(
      size, size, false, 1, bgfx::TextureFormat::BGRA8,
      BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  if (!bgfx::isValid(output) || !bgfx::isValid(readback) ||
      !bgfx::isValid(framebuffer)) return false;
  bool passed = true;
  float ortho[16];
  bx::mtxOrtho(ortho, 0, 320, 160, 0, 0, 100, 0,
               bgfx::getCaps()->homogeneousDepth);
  bgfx::setViewFrameBuffer(rendering::ui_view, framebuffer);
  bgfx::setViewRect(rendering::ui_view, 0, 0, size, size);
  bgfx::setViewTransform(rendering::ui_view, nullptr, ortho);
  bgfx::setViewMode(rendering::ui_view, bgfx::ViewMode::Sequential);
  bgfx::setViewClear(rendering::ui_view, BGFX_CLEAR_COLOR, 0x193149ffU);
  {
    rendering::UiBatchRenderer renderer;
    RenderContext context(renderer);
    struct FontCase { const char *path; TextView::FontWeight weight; };
    for (const auto [font, weight] : {
             FontCase{app_theme::font, TextView::FontWeight::Regular},
             FontCase{app_theme::font, TextView::FontWeight::Bold},
             FontCase{"assets/fonts/notosanscjkjp.ttf", TextView::FontWeight::Regular},
             FontCase{"assets/fonts/notosanscjkjp.ttf", TextView::FontWeight::Bold}}) {
      TextView reference(font, 24, weight);
      TextView atlas(font, 24, weight);
      atlas.setGlyphAtlasEnabled(true);
      for (auto *view : {&reference, &atlas}) {
        view->setSize(290, 100);
        view->setAlign(TextView::CENTER);
        view->setVAlign(TextView::MIDDLE);
      }
      struct TextCase { const char *value; bool numeric; float rotation = 0; };
      for (const auto [value, numeric, rotation] : {
               TextCase{"SCORE 0123456789", false}, TextCase{"GREAT 321", false},
               TextCase{"+15.9 ms", true}, TextCase{"100.0%", true},
               TextCase{"AV To", false}, TextCase{"999999", true, 13},
               TextCase{"T", false}, TextCase{"_", false},
               TextCase{"123\n456", false}, TextCase{"日本語", false}}) {
        for (const SDL_Color color : {SDL_Color{255, 255, 255, 255},
                                      SDL_Color{31, 173, 229, 127},
                                      SDL_Color{220, 101, 17, 0}}) {
          std::array<std::vector<std::uint8_t>, 2> images;
          int index = 0;
          for (auto *view : {&reference, &atlas}) {
            view->setText(value);
            view->setColor(color);
            view->setRotationDegrees(rotation);
            view->setMinimumFontSize(rotation == 0 ? 0 : 12);
            view->setSize(rotation == 0 ? 290 : 60, 100);
            renderer.beginFrame();
            {
              RenderContext::UiBatchScope batch(context);
              RenderContext::OpacityScope opacity(context, color.a == 127 ? 0.6f : 1.0f);
              view->render(context);
            }
            bgfx::touch(rendering::ui_view);
            bgfx::blit(rendering::readback_view, readback, 0, 0, output);
            auto frame = bgfx::frame();
            auto &pixels = images[index++];
            pixels.resize(size * size * 4);
            const auto ready = bgfx::readTexture(readback, pixels.data());
            for (int guard = 0; frame < ready && guard < 16; ++guard)
              frame = bgfx::frame();
            if (ready == std::numeric_limits<std::uint32_t>::max() || frame < ready)
              passed = false;
          }
          if (atlas.textureWidth() != reference.textureWidth() ||
              atlas.textureHeight() != reference.textureHeight()) passed = false;
          int maximumDifference = 0;
          for (std::size_t i = 0; i < images[0].size(); ++i)
            maximumDifference = std::max(maximumDifference,
                std::abs(int(images[0][i]) - int(images[1][i])));
          if (maximumDifference > 1) {
            std::cerr << "FAIL: atlas pixels differ for " << value << " by "
                      << maximumDifference << '\n';
            passed = false;
          }
          // Tight kerning can overlap coverage; those strings intentionally
          // retain surface compositing. Changing numeric labels use the atlas.
          if (numeric && bgfx::isValid(atlas.textureHandle())) {
            std::cerr << "FAIL: changing label uploaded a whole-string texture: "
                      << font << ' ' << value << '\n';
            passed = false;
          }
        }
      }
      // Unsupported multiline content must still render through the old path.
      atlas.setText("123\n456");
      renderer.beginFrame();
      { RenderContext::UiBatchScope batch(context); atlas.render(context); }
      if (!bgfx::isValid(atlas.textureHandle())) passed = false;
      bgfx::frame();
    }
  }
  bgfx::setViewFrameBuffer(rendering::ui_view, BGFX_INVALID_HANDLE);
  bgfx::destroy(framebuffer);
  bgfx::destroy(readback);
  bgfx::destroy(output);
  return passed;
}

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
  const bool passed = testIconEdgesDoNotWrap() && testChangingTextUsesGlyphAtlas() &&
                      testGlyphTexturesAreSharedAndReused();
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::frame();
  bgfx::frame();
  bgfx::shutdown();
  return passed ? 0 : 1;
}
