#include "view/Button.h"
#include "view/CheckboxButtonContent.h"
#include "view/PlayOptionsPanelView.h"
#include "view/ScrollView.h"
#include "view/TextView.h"
#include "rendering/UniformCache.h"

#include <SDL2/SDL.h>

#include <cmath>
#include <cstdlib>
#include <iostream>

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
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void click(Button &button) {
  SDL_Event down{};
  down.type = SDL_MOUSEBUTTONDOWN;
  down.button.type = SDL_MOUSEBUTTONDOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.x = button.getX() + button.getWidth() / 2;
  down.button.y = button.getY() + button.getHeight() / 2;
  SDL_Event up = down;
  up.type = SDL_MOUSEBUTTONUP;
  up.button.type = SDL_MOUSEBUTTONUP;
  button.handleEvents(down);
  button.handleEvents(up);
}

const TextView *buttonText(const Button &button) {
  return dynamic_cast<const TextView *>(button.getContentView());
}

void testScrollViewUsesPreciseWheelDeltaAndNaturalDirection() {
  ScrollView scroll(0, 0, 360, 200);
  auto *content = new View();
  content->setWidth(360)->setHeight(800);
  scroll.setContentView(content);
  scroll.applyYogaLayout();

  SDL_Event normal{};
  normal.type = SDL_MOUSEWHEEL;
  normal.wheel.type = SDL_MOUSEWHEEL;
  normal.wheel.y = 0;
  normal.wheel.preciseY = 0.25F;
  normal.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  scroll.setScrollOffset(100.0F);
  scroll.handleEvents(normal);
  require(std::abs(scroll.getScrollOffset() - 88.0F) < 0.001F,
          "scroll view uses a fractional normal wheel delta");

  SDL_Event natural = normal;
  natural.wheel.preciseY = -0.25F;
  natural.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
  scroll.setScrollOffset(100.0F);
  scroll.handleEvents(natural);
  require(std::abs(scroll.getScrollOffset() - 112.0F) < 0.001F,
          "scroll view preserves the iPad natural-scroll direction");
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for panel resources");

  testScrollViewUsesPreciseWheelDeltaAndNaturalDirection();

  {
    GameplayRuleset selected = GameplayRuleset::LR2;
    ScrollView scroll(0, 0, 360, 400);
    scroll.setWidth(360)->setHeight(400);
    auto *content = new View();
    content->setWidth(340)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch);
    auto *panel = new PlayOptionsPanelView(
        {.onRulesetSelected =
             [&](GameplayRuleset ruleset) { selected = ruleset; }},
        {.width = 340.0f,
         .playOptionColumns = 2,
         .showGauge = true,
         .showLaneOrder = false,
         .showPacemaker = true},
        nullptr);
    content->addView(panel);
    scroll.setContentView(content);
    scroll.applyYogaLayout();

    panel->refresh({.ruleset = GameplayRuleset::LR2});
    auto *lr2 = dynamic_cast<Button *>(panel->findViewByName("ruleset-lr2"));
    auto *beatoraja =
        dynamic_cast<Button *>(panel->findViewByName("ruleset-beatoraja"));
    require(lr2 != nullptr && beatoraja != nullptr,
            "ruleset buttons have stable names");
    require(buttonText(*lr2) != nullptr && buttonText(*beatoraja) != nullptr &&
                buttonText(*lr2)->getText() == "LR2" &&
                buttonText(*beatoraja)->getText() == "Beatoraja",
            "ruleset buttons have the approved labels");
    require(lr2->isSelected() && !beatoraja->isSelected(),
            "LR2 selected styling follows panel state");

    auto *clubMode =
        dynamic_cast<Button *>(panel->findViewByName("club-mode"));
    auto *clubContent = dynamic_cast<CheckboxButtonContent *>(
        clubMode == nullptr ? nullptr : clubMode->getContentView());
    require(clubContent != nullptr && !clubContent->checked(),
            "Club Beat starts with the FontAwesome unchecked icon");
    panel->refresh({.ruleset = GameplayRuleset::LR2, .clubMode = true});
    require(clubContent->checked(),
            "Club Beat refresh switches to the FontAwesome checked icon");

    click(*beatoraja);
    require(selected == GameplayRuleset::Beatoraja,
            "Beatoraja button invokes the enum callback");
    panel->refresh({.ruleset = GameplayRuleset::Beatoraja});
    require(!lr2->isSelected() && beatoraja->isSelected(),
            "Beatoraja selected styling follows panel state");

    auto *rulesetLabel = panel->findViewByName("ruleset-section-label");
    auto *gaugeLabel = panel->findViewByName("gauge-section-label");
    require(rulesetLabel != nullptr && gaugeLabel != nullptr &&
                rulesetLabel->getY() < gaugeLabel->getY(),
            "Ruleset appears before Gauge in scroll content");
    require(lr2->getWidth() == beatoraja->getWidth() && lr2->getWidth() > 0,
            "compact layout keeps equal-width ruleset buttons");
    require(scroll.getHeight() == 400,
            "adding the ruleset row does not grow the modal viewport");
    scroll.scrollToBottom();
    require(scroll.getScrollOffset() > 0.0f,
            "the compact modal content remains scrollable");
  }
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
