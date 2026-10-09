#include "rendering/UniformCache.h"
#include "rendering/common.h"
#include "scene/NewcomerTutorialView.h"
#include "view/IconText.h"
#include "view/TextView.h"
#include "view/Button.h"

#include <bgfx/bgfx.h>

#include <iostream>
#include <string>
#include <vector>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering


namespace {
void require(bool condition, const char *message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

void testLanguagePrecedesTourAndCompletionRequiresSave() {
  bool savesSucceed = false;
  std::string preference;
  bool completed = false;
  NewcomerTutorialView tour({
      .saveLanguage = [&](const std::string &value) {
        if (!savesSucceed) return false;
        preference = value;
        i18n::setLanguage(i18n::resolveLanguage(value, {}));
        return true;
      },
      .complete = [&] { completed = savesSucceed; return completed; }});
  require(tour.step() == NewcomerTutorialStep::Language, "language must come first");
  tour.chooseLanguage("ko");
  tour.advance();
  require(tour.step() == NewcomerTutorialStep::Language, "failed language save keeps chooser open");
  savesSucceed = true;
  tour.chooseLanguage("ko");
  tour.advance();
  require(preference == "ko" && tour.step() == NewcomerTutorialStep::Tables,
          "chosen language must be saved before explaining difficulty tables");
  tour.advance();
  require(tour.step() == NewcomerTutorialStep::Download, "explain Find BMS after tables");
  tour.back();
  require(tour.step() == NewcomerTutorialStep::Tables, "Back revisits the previous tip");
  tour.advance();
  tour.advance();
  require(tour.step() == NewcomerTutorialStep::Folder, "explain importing a library folder");
  tour.advance();
  require(tour.step() == NewcomerTutorialStep::PlayOptions, "explain play options last");
  savesSucceed = false;
  tour.advance();
  require(tour.getVisible() && !completed, "failed completion save keeps tutorial retryable");
  savesSucceed = true;
  tour.advance();
  require(!tour.getVisible() && completed, "Finish persists completion and dismisses tutorial");
}

void testChineseLanguageChoicesAndInitialSelection() {
  for (const auto preference : {"zh-Hans", "zh-Hant"}) {
    i18n::setLanguage(i18n::resolveLanguage(preference, {}));
    std::string saved;
    NewcomerTutorialView tour({
        .saveLanguage = [&](const std::string &value) {
          saved = value;
          i18n::setLanguage(i18n::resolveLanguage(value, {}));
          return true;
        }});
    tour.advance();
    require(saved == preference, "onboarding preserves the system's Chinese variant");
    tour.back();
    for (const auto key : {SDLK_4, SDLK_5}) {
      SDL_Event event{};
      event.type = SDL_EVENT_KEY_DOWN;
      event.key.key = key;
      tour.handleEvents(event);
      require(saved == (key == SDLK_4 ? "zh-Hans" : "zh-Hant"),
              "Chinese keyboard choices persist their distinct language codes");
    }
  }
}

void testSkipAndInputBlocking() {
  bool completed = false;
  NewcomerTutorialView tour({
      .saveLanguage = [](const std::string &) { return true; },
      .complete = [&] { completed = true; return true; }});
  tour.updateLayout(1280, 720);
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.x = 4;
  event.button.y = 4;
  require(!tour.handleEvents(event), "tutorial must block clicks through the backdrop");
  tour.skip();
  require(completed && !tour.getVisible(), "Skip persists dismissal too");
  require(tour.handleEvents(event), "dismissed tutorial releases input");
}

void testTranslatedTipsFitAndLeaveTargetsVisible() {
  for (const auto language : {i18n::Language::English, i18n::Language::Korean,
                              i18n::Language::Japanese, i18n::Language::SimplifiedChinese,
                              i18n::Language::TraditionalChinese}) {
    i18n::setLanguage(language);
    for (const auto width : {1920, 1280, 800, 390}) {
      const int height = width == 390 ? 844 : 720;
      rendering::window_width = width;
      rendering::window_height = height;
      View target(20, 100, 200, 580);
      NewcomerTutorialView tour({
          .saveLanguage = [](const std::string &) { return true; },
          .complete = [] { return true; },
          .target = [&](NewcomerTutorialStep) { return &target; }});
      // Portrait coverage exercises the language chooser without a spotlight.
      for (int step = 0; step <= (width == 390 ? 0 : 4); ++step) {
        tour.updateLayout(width, height);
        const auto *panel = tour.getChildren().back();
        require(panel->getX() >= 0 && panel->getY() >= 0 &&
                    panel->getX() + panel->getWidth() <= width &&
                    panel->getY() + panel->getHeight() <= height,
                "tutorial card must remain inside the viewport");
        require(step == 0 || panel->getX() >= target.getX() + target.getWidth() ||
                    panel->getX() + panel->getWidth() <= target.getX(),
                "tutorial card must not cover the highlighted control");
        const auto checkText = [&](auto &&self, View *parent) -> void {
          for (auto *child : parent->getChildren()) {
            if (!child->getVisible()) continue;
            if (const auto *text = dynamic_cast<TextView *>(child)) {
              if (text->textureWidth() > text->getWidth() ||
                  text->textureHeight() > text->getHeight()) {
                std::cerr << "Overflow at width " << width << ": " << text->getText()
                          << " texture " << text->textureWidth() << "x" << text->textureHeight()
                          << " view " << text->getWidth() << "x" << text->getHeight() << "\n";
              }
              require(text->textureWidth() <= text->getWidth() &&
                          text->textureHeight() <= text->getHeight(),
                      "translated tutorial text must wrap within its view");
              require(text->getY() + text->getHeight() <= parent->getY() + parent->getHeight(),
                      "wrapped text must fit its allocated space without covering controls");
            }
            self(self, child);
          }
        };
        checkText(checkText, tour.getChildren().back());
        tour.advance();
      }
    }
  }
}

void testSpotlightDoesNotActivateTheUnderlyingButton() {
  rendering::window_width = 1280;
  rendering::window_height = 720;
  View root(0, 0, 1280, 720);
  auto *target = new Button(20, 100, 200, 60);
  target->setPositionType(YGPositionTypeAbsolute);
  int clicks = 0;
  target->setOnClickListener([&] { ++clicks; });
  root.addView(target);
  auto *tour = new NewcomerTutorialView({
      .saveLanguage = [](const std::string &) { return true; },
      .complete = [] { return true; },
      .target = [&](NewcomerTutorialStep) { return target; }});
  root.addView(tour);
  SDL_Event enter{};
  enter.type = SDL_EVENT_KEY_DOWN;
  enter.key.key = SDLK_RETURN;
  require(!root.handleEvents(enter) && tour->step() == NewcomerTutorialStep::Tables,
          "Enter advances the tour while consuming the input");
  tour->updateLayout(1280, 720);
  SDL_Event click{};
  click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  click.button.button = SDL_BUTTON_LEFT;
  click.button.x = target->getX() + 10;
  click.button.y = target->getY() + 10;
  root.handleEvents(click);
  click.type = SDL_EVENT_MOUSE_BUTTON_UP;
  root.handleEvents(click);
  require(clicks == 0, "even the clear spotlight must block downloads and folder actions");
  tour->skip();
  click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  root.handleEvents(click);
  click.type = SDL_EVENT_MOUSE_BUTTON_UP;
  root.handleEvents(click);
  require(clicks == 1, "normal control interaction resumes after the tour");
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  if (!bgfx::init(init)) return 1;
  testLanguagePrecedesTourAndCompletionRequiresSave();
  testChineseLanguageChoicesAndInitialSelection();
  testSkipAndInputBlocking();
  testTranslatedTipsFitAndLeaveTargetsVisible();
  testSpotlightDoesNotActivateTheUnderlyingButton();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
