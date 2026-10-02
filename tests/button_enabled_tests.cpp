#include "../src/view/View.h"
#include <functional>
#include <memory>
#include <string>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "../src/view/Button.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstdlib>
#include <iostream>
#include <vector>
#include "../src/rendering/UniformCache.h"
#include <bx/math.h>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0f;
float heightScale = 1.0f;
float ui_scale_x = 1.0f;
float ui_scale_y = 1.0f;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {
#define REQUIRE(condition) require((condition), #condition, __LINE__)

void require(bool condition, const char *expression, int line) {
  if (condition) {
    return;
  }
  std::cerr << "requirement failed at line " << line << ": " << expression
            << '\n';
  std::exit(1);
}

SDL_Event mouseEvent(Uint32 type, int x, int y) {
  SDL_Event event{};
  event.type = type;
  event.button.type = type;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.which = 1;
  event.button.x = x;
  event.button.y = y;
  return event;
}

SDL_Event mouseSynthesizedFingerEvent(Uint32 type, float x, float y) {
  SDL_Event event{};
  event.type = type;
  event.tfinger.type = type;
  event.tfinger.touchId = SDL_MOUSE_TOUCHID;
  event.tfinger.fingerId = 0;
  event.tfinger.x = x;
  event.tfinger.y = y;
  return event;
}

SDL_Event mouseMotion(Uint32 which, int x, int y) {
  SDL_Event event{};
  event.type = SDL_MOUSEMOTION;
  event.motion.which = which;
  event.motion.x = x;
  event.motion.y = y;
  return event;
}

SDL_Event fingerEvent(Uint32 type, int x, int y) {
  SDL_Event event{};
  event.type = type;
  event.tfinger.touchId = 1;
  event.tfinger.fingerId = 7;
  event.tfinger.x = static_cast<float>(x) / rendering::window_width;
  event.tfinger.y = static_cast<float>(y) / rendering::window_height;
  return event;
}

struct CapturedSubmission {
  std::vector<rendering::PosColorVertex> colors;
  rendering::UiBatchState state;
  bool textured = false;
};

class RecordingBackend final : public rendering::UiBatchBackend {
public:
  bool submit(const rendering::UiBatchSubmission &submission) noexcept override {
    submissions.push_back({
        .colors = {submission.colorVertices.begin(),
                   submission.colorVertices.end()},
        .state = submission.state,
        .textured = submission.format ==
                    rendering::UiBatchVertexFormat::Textured});
    return true;
  }
  std::vector<CapturedSubmission> submissions;
};

class MixedContentView final : public View {
public:
  explicit MixedContentView(bgfx::TextureHandle texture = {5},
                            bgfx::UniformHandle sampler = {6},
                            bool imageFade = false)
      : texture(texture), sampler(sampler), imageFade(imageFade) {}

private:
  void renderImpl(RenderContext &context) override {
    const std::array colors = {
        rendering::PosColorVertex{0, 0, 0, 0xc01e140aU},
        rendering::PosColorVertex{20, 0, 0, 0xc01e140aU},
        rendering::PosColorVertex{20, 20, 0, 0xc01e140aU},
        rendering::PosColorVertex{0, 20, 0, 0xc01e140aU}};
    constexpr std::array<std::uint16_t, 6> indices = {0, 1, 2, 0, 2, 3};
    context.appendUiColor(
        colors, indices,
        context.makeUiBatchState(
            rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE),
            BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA));
    const std::array texturedVertices = {
        rendering::PosTexCoord0Vertex{20, 0, 0, 0, 0},
        rendering::PosTexCoord0Vertex{40, 0, 0, 1, 0},
        rendering::PosTexCoord0Vertex{40, 20, 0, 1, 1},
        rendering::PosTexCoord0Vertex{20, 20, 0, 0, 1}};
    auto state = context.makeUiBatchState(
        rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT),
        BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
    state.texture = texture;
    state.sampler = sampler;
    state.samplerFlags = BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP |
                         BGFX_SAMPLER_V_CLAMP;
    if (imageFade) {
      state.program = rendering::ShaderManager::getInstance().getProgram(
          "vs_text.bin", "fs_image_fade.bin");
      state.uniforms[0] = {
          .handle = rendering::UniformCache::getInstance().getVec4(
              "u_imageFadeParams"),
          .value = {1.0F, 0.0F, 0.0F, 0.5F}};
      state.uniforms[1] = {
          .handle = rendering::UniformCache::getInstance().getVec4(
              "u_imageScrimColor"),
          .value = {0.0F, 0.0F, 0.0F, 0.0F}};
      state.uniformCount = 2;
    }
    context.appendUiTextured(texturedVertices, indices, state);
  }
  bgfx::TextureHandle texture;
  bgfx::UniformHandle sampler;
  bool imageFade;
};

void testDisabledButtonDimsWholeContentAndRestoresFollowingViews() {
  RecordingBackend backend;
  rendering::UiBatchRenderer renderer(backend);
  RenderContext context(renderer);
  Button button(0, 0, 100, 50);
  button.setBackgroundColors(Color(40, 50, 60, 200),
                             Color(80, 90, 100, 255),
                             Color(120, 130, 140, 255));
  button.setBorderColors(Color(70, 80, 90, 255), Color(1, 2, 3, 255),
                         Color(4, 5, 6, 255));
  button.setStyledBorderWidth(2);
  button.setContentView(new MixedContentView());
  MixedContentView sibling;
  sibling.setSize(100, 50);
  auto hover = mouseMotion(1, 10, 10);
  button.handleEvents(hover);
  auto down = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  button.handleEvents(down);
  button.setEnabled(false);
  {
    RenderContext::UiBatchScope batch(context);
    button.render(context);
    sibling.render(context);
  }

  REQUIRE(backend.submissions.size() == 5);
  const auto &decoration = backend.submissions[0].colors;
  REQUIRE(decoration.size() == 8);
  REQUIRE(decoration[0].abgr == 0x725a5046U);
  REQUIRE(decoration[4].abgr == 0x5a3c3228U);
  REQUIRE(backend.submissions[1].colors[0].abgr == 0x561e140aU);
  const auto &disabledTexture = backend.submissions[2];
  REQUIRE(disabledTexture.textured);
  REQUIRE(disabledTexture.state.uniformCount == 1);
  REQUIRE(disabledTexture.state.textureOpacity == 0.45F);
  REQUIRE(disabledTexture.state.uniforms[0].value ==
          (std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F}));
  REQUIRE(disabledTexture.state.program.idx !=
          backend.submissions[4].state.program.idx);
  REQUIRE(backend.submissions[3].colors[0].abgr == 0xc01e140aU);
  REQUIRE(backend.submissions[4].state.uniformCount == 0);

  backend.submissions.clear();
  button.setEnabled(true);
  {
    RenderContext::UiBatchScope batch(context);
    button.render(context);
  }
  REQUIRE(backend.submissions.size() == 3);
  REQUIRE(backend.submissions[0].colors[0].abgr == 0xff5a5046U);
  REQUIRE(backend.submissions[0].colors[4].abgr == 0xc83c3228U);
  REQUIRE(backend.submissions[1].colors[0].abgr == 0xc01e140aU);
  REQUIRE(backend.submissions[2].state.uniformCount == 0);
}

void testDisabledButtonContentPixels() {
  const auto output = bgfx::createTexture2D(
      64, 64, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(
      64, 64, false, 1, bgfx::TextureFormat::BGRA8,
      BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  // Transparent white and half-alpha white expose alpha loss or an opaque
  // rectangle around disabled image/text content.
  const std::array<std::uint32_t, 2> texels = {0x00ffffffU, 0x80ffffffU};
  const auto texture = bgfx::createTexture2D(
      2, 1, false, 1, bgfx::TextureFormat::RGBA8, 0,
      bgfx::copy(texels.data(), sizeof(texels)));
  REQUIRE(bgfx::isValid(output));
  REQUIRE(bgfx::isValid(readback));
  REQUIRE(bgfx::isValid(framebuffer));
  REQUIRE(bgfx::isValid(texture));
  float ortho[16];
  bx::mtxOrtho(ortho, 0, 64, 64, 0, 0, 100, 0,
               bgfx::getCaps()->homogeneousDepth);
  bgfx::setViewFrameBuffer(rendering::ui_view, framebuffer);
  bgfx::setViewRect(rendering::ui_view, 0, 0, 64, 64);
  bgfx::setViewTransform(rendering::ui_view, nullptr, ortho);
  bgfx::setViewMode(rendering::ui_view, bgfx::ViewMode::Sequential);
  bgfx::setViewClear(rendering::ui_view, BGFX_CLEAR_COLOR, 0x000000ffU);
  const auto sampler =
      rendering::UniformCache::getInstance().getSampler("s_texColor");
  {
    rendering::UiBatchRenderer renderer;
    RenderContext context(renderer);
    Button button(0, 0, 50, 30);
    for (const bool imageFade : {false, true}) {
      button.setContentView(new MixedContentView(texture, sampler, imageFade));
      for (const bool enabled : {false, true, false, true}) {
        button.setEnabled(enabled);
        renderer.beginFrame();
        {
          RenderContext::UiBatchScope batch(context);
          button.render(context);
        }
        bgfx::touch(rendering::ui_view);
        bgfx::blit(rendering::readback_view, readback, 0, 0, output);
        auto frame = bgfx::frame();
        std::vector<std::uint8_t> pixels(64 * 64 * 4);
        const auto readyFrame = bgfx::readTexture(readback, pixels.data());
        REQUIRE(readyFrame != std::numeric_limits<std::uint32_t>::max());
        for (int guard = 0; frame < readyFrame && guard < 16; ++guard) {
          frame = bgfx::frame();
        }
        REQUIRE(frame >= readyFrame);
        const auto channel = [&](int x, int channel) {
          return static_cast<int>(pixels[(10 * 64 + x) * 4 + channel]);
        };
        REQUIRE(std::abs(channel(10, 0) - (enabled ? 23 : 10)) <= 1);
        REQUIRE(std::abs(channel(10, 1) - (enabled ? 15 : 7)) <= 1);
        REQUIRE(std::abs(channel(10, 2) - (enabled ? 8 : 3)) <= 1);
        for (int rgb = 0; rgb < 3; ++rgb) {
          REQUIRE(channel(25, rgb) == 0);
          // At x=35 the pixel center has UV progress 15.5/20=.775;
          // strength .5 gives .8875 alpha, then disabled opacity .45.
          const int expected = imageFade ? (enabled ? 114 : 51)
                                         : (enabled ? 128 : 58);
          REQUIRE(std::abs(channel(35, rgb) - expected) <= 1);
        }
      }
    }
  }
  bgfx::setViewFrameBuffer(rendering::ui_view, BGFX_INVALID_HANDLE);
  bgfx::destroy(framebuffer);
  bgfx::destroy(output);
  bgfx::destroy(readback);
  bgfx::destroy(texture);
}

void testTouchReleaseClearsHoverBeforeClick() {
  for (const bool releaseInside : {true, false}) {
    Button button(0, 0, 100, 50);
    int clicks = 0;
    button.setOnClickListener([&]() {
      REQUIRE(!button.isHovered);
      REQUIRE(button.activeTouchId == -1);
      ++clicks;
    });
    auto mouse = mouseMotion(1, 10, 10);
    button.handleEvents(mouse);
    REQUIRE(button.isHovered);
    auto down = fingerEvent(SDL_FINGERDOWN, 10, 10);
    REQUIRE(!button.handleEvents(down));
    auto syntheticMotion = mouseMotion(SDL_TOUCH_MOUSEID, 10, 10);
    button.handleEvents(syntheticMotion);
    auto up = fingerEvent(SDL_FINGERUP, releaseInside ? 10 : 150, 10);
    REQUIRE(!button.handleEvents(up));
    REQUIRE(!button.isHovered);
    REQUIRE(button.activeTouchId == -1);
    REQUIRE(clicks == (releaseInside ? 1 : 0));

    // SDL may deliver its touch-generated mouse motion after finger-up.
    button.handleEvents(syntheticMotion);
    REQUIRE(!button.isHovered);
    button.handleEvents(mouse);
    REQUIRE(button.isHovered);
    auto mouseLeave = mouseMotion(1, 150, 10);
    button.handleEvents(mouseLeave);
    REQUIRE(!button.isHovered);
  }
}

void click(Button &button) {
  auto down = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  auto up = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
  button.handleEvents(down);
  button.handleEvents(up);
}

void testDisabledButtonBlocksUnderlyingPointerActions() {
  View stack(0, 0, 200, 100);
  int underlyingClicks = 0;
  int disabledClicks = 0;
  auto *underlying = new Button(0, 0, 200, 100);
  underlying->setOnClickListener([&]() { ++underlyingClicks; });
  stack.addView(underlying);
  auto *disabled = new Button(0, 0, 100, 50);
  disabled->setPosition(0, 0, YGPositionTypeAbsolute);
  disabled->setOnClickListener([&]() { ++disabledClicks; });
  disabled->setEnabled(false);
  stack.addView(disabled);

  for (auto event : {mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10),
                     mouseEvent(SDL_MOUSEBUTTONUP, 10, 10),
                     fingerEvent(SDL_FINGERDOWN, 10, 10),
                     fingerEvent(SDL_FINGERUP, 10, 10)}) {
    REQUIRE(!stack.handleEvents(event));
  }
  REQUIRE(underlyingClicks == 0);
  REQUIRE(disabledClicks == 0);
  for (auto event : {mouseMotion(1, 10, 10),
                     fingerEvent(SDL_FINGERMOTION, 10, 10)}) {
    REQUIRE(disabled->handleEvents(event));
    stack.handleEvents(event);
  }
  REQUIRE(underlying->isHovered);
  REQUIRE(!disabled->isHovered);
  REQUIRE(!disabled->mousePressedInside);
  REQUIRE(disabled->activeTouchId == -1);

  auto outsideDown = mouseEvent(SDL_MOUSEBUTTONDOWN, 150, 10);
  auto outsideUp = mouseEvent(SDL_MOUSEBUTTONUP, 150, 10);
  stack.handleEvents(outsideDown);
  stack.handleEvents(outsideUp);
  REQUIRE(underlyingClicks == 1);

  disabled->setEnabled(true);
  auto insideDown = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  auto insideUp = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
  stack.handleEvents(insideDown);
  stack.handleEvents(insideUp);
  REQUIRE(disabledClicks == 1);
  REQUIRE(underlyingClicks == 1);
}

void testDisablingCancelsHoverAndActivePointerGestures() {
  Button button(0, 0, 100, 50);
  int clicks = 0;
  button.setOnClickListener([&]() { ++clicks; });
  auto motion = mouseMotion(1, 10, 10);
  button.handleEvents(motion);
  auto mouseDown = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  button.handleEvents(mouseDown);
  button.setEnabled(false);
  button.setEnabled(true);
  auto mouseUp = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
  button.handleEvents(mouseUp);
  REQUIRE(clicks == 0);
  REQUIRE(!button.isHovered);
  REQUIRE(!button.mousePressedInside);

  auto fingerDown = fingerEvent(SDL_FINGERDOWN, 10, 10);
  button.handleEvents(fingerDown);
  button.setEnabled(false);
  button.setEnabled(true);
  auto fingerUp = fingerEvent(SDL_FINGERUP, 10, 10);
  button.handleEvents(fingerUp);
  REQUIRE(clicks == 0);
  REQUIRE(button.activeTouchId == -1);
  click(button);
  REQUIRE(clicks == 1);
}

void testDisabledSiblingDoesNotLeaveCoveredButtonGesturesStuck() {
  for (const bool separateContent : {false, true}) {
    for (const int enabledWidth : {100, 200}) {
      View stack(0, 0, 200, 50);
      auto *enabled = new Button(0, 0, enabledWidth, 50);
      int clicks = 0;
      enabled->setOnClickListener([&]() { ++clicks; });
      if (separateContent) {
        auto *wrapper = new Button(0, 0, enabledWidth, 50);
        wrapper->setContentView(enabled);
        stack.addView(wrapper);
      } else {
        auto *wrapper = new View(0, 0, enabledWidth, 50);
        wrapper->addView(enabled);
        stack.addView(wrapper);
      }
      auto *disabled = new Button(100, 0, 100, 50);
      disabled->setPosition(100, 0, YGPositionTypeAbsolute);
      disabled->setEnabled(false);
      stack.addView(disabled);

      auto hover = mouseMotion(1, 10, 10);
      stack.handleEvents(hover);
      REQUIRE(enabled->isHovered);
      auto down = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
      REQUIRE(!stack.handleEvents(down));
      REQUIRE(enabled->mousePressedInside);
      auto coveredMotion = mouseMotion(1, 110, 10);
      REQUIRE(stack.handleEvents(coveredMotion));
      REQUIRE(enabled->isHovered == (enabledWidth == 200));
      // Retain the initiating press until release, as when dragging outside
      // an ordinary button; a covered release must clear it without a click.
      REQUIRE(enabled->mousePressedInside);
      auto coveredUp = mouseEvent(SDL_MOUSEBUTTONUP, 110, 10);
      REQUIRE(!stack.handleEvents(coveredUp));
      REQUIRE(!enabled->mousePressedInside);
      REQUIRE(clicks == 0);

      auto touchDown = fingerEvent(SDL_FINGERDOWN, 10, 10);
      REQUIRE(!stack.handleEvents(touchDown));
      REQUIRE(enabled->activeTouchId == 7);
      auto otherTouchUp = fingerEvent(SDL_FINGERUP, 110, 10);
      otherTouchUp.tfinger.fingerId = 8;
      REQUIRE(!stack.handleEvents(otherTouchUp));
      REQUIRE(enabled->activeTouchId == 7);
      auto coveredTouchUp = fingerEvent(SDL_FINGERUP, 110, 10);
      REQUIRE(!stack.handleEvents(coveredTouchUp));
      REQUIRE(enabled->activeTouchId == -1);
      REQUIRE(clicks == 0);

      stack.handleEvents(hover);
      stack.handleEvents(down);
      auto up = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
      stack.handleEvents(up);
      REQUIRE(clicks == 1);
    }
  }
}
} // namespace

int main(int argc, char **argv) {
  const bool renderOnly = argc == 2 && std::string(argv[1]) == "--render-only";
  const bool metalOnly = argc == 2 && std::string(argv[1]) == "--metal-only";
  if (!renderOnly && !metalOnly) {
    testDisabledSiblingDoesNotLeaveCoveredButtonGesturesStuck();
    testDisabledButtonBlocksUnderlyingPointerActions();
    testDisablingCancelsHoverAndActivePointerGestures();
  }
  bgfx::Init init;
  init.type = metalOnly ? bgfx::RendererType::Metal : bgfx::RendererType::Noop;
  init.fallback = false;
  init.resolution.width = metalOnly ? 0 : 64;
  init.resolution.height = metalOnly ? 0 : 64;
  REQUIRE(bgfx::init(init));
  if (metalOnly) {
    testDisabledButtonContentPixels();
  } else {
    testDisabledButtonDimsWholeContentAndRestoresFollowingViews();
  }
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  if (renderOnly || metalOnly) {
    return 0;
  }
  testTouchReleaseClearsHoverBeforeClick();
  Button button(0, 0, 100, 50);
  int clicks = 0;
  button.setOnClickListener([&]() { ++clicks; });

  button.setEnabled(false);
  REQUIRE(!button.isEnabled());
  click(button);
  REQUIRE(clicks == 0);

  button.setEnabled(true);
  REQUIRE(button.isEnabled());
  click(button);
  REQUIRE(clicks == 1);

  auto down = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  auto up = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
  button.handleEvents(down);
  button.setEnabled(false);
  button.handleEvents(up);
  REQUIRE(clicks == 1);

  Button trackpadButton(0, 0, 100, 50);
  int trackpadClicks = 0;
  trackpadButton.setOnClickListener([&]() { ++trackpadClicks; });
  auto syntheticDown =
      mouseSynthesizedFingerEvent(SDL_FINGERDOWN, 0.005F, 0.01F);
  auto mouseDown = mouseEvent(SDL_MOUSEBUTTONDOWN, 10, 10);
  auto syntheticUp =
      mouseSynthesizedFingerEvent(SDL_FINGERUP, 0.005F, 0.01F);
  auto mouseUp = mouseEvent(SDL_MOUSEBUTTONUP, 10, 10);
  trackpadButton.handleEvents(syntheticDown);
  trackpadButton.handleEvents(mouseDown);
  trackpadButton.handleEvents(syntheticUp);
  trackpadButton.handleEvents(mouseUp);
  REQUIRE(trackpadClicks == 1);
  return 0;
}
