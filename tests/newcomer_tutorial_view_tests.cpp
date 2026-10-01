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

void testSkipAndInputBlocking() {
  bool completed = false;
  NewcomerTutorialView tour({
      .saveLanguage = [](const std::string &) { return true; },
      .complete = [&] { completed = true; return true; }});
  tour.updateLayout(1280, 720);
  SDL_Event event{};
  event.type = SDL_MOUSEBUTTONDOWN;
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
                              i18n::Language::Japanese}) {
    i18n::setLanguage(language);
    for (const auto width : {1920, 1280, 800}) {
      rendering::window_width = width;
      rendering::window_height = 720;
      View target(20, 100, 200, 580);
      NewcomerTutorialView tour({
          .saveLanguage = [](const std::string &) { return true; },
          .complete = [] { return true; },
          .target = [&](NewcomerTutorialStep) { return &target; }});
      tour.advance();
      for (int step = 1; step <= 4; ++step) {
        tour.updateLayout(width, 720);
        const auto *panel = tour.getChildren().back();
        require(panel->getX() >= 0 && panel->getY() >= 0 &&
                    panel->getX() + panel->getWidth() <= width &&
                    panel->getY() + panel->getHeight() <= 720,
                "tutorial card must remain inside the viewport");
        require(panel->getX() >= target.getX() + target.getWidth() ||
                    panel->getX() + panel->getWidth() <= target.getX(),
                "tutorial card must not cover the highlighted control");
        for (auto *child : tour.getChildren().back()->getChildren()) {
          const auto *text = dynamic_cast<TextView *>(child);
          if (text && text->getVisible()) {
            if (text->textureHeight() > text->getHeight()) {
              std::cerr << "Clipped tip at width " << width << ": " << text->getText()
                        << " (" << text->textureHeight() << " > " << text->getHeight() << ")\n";
            }
            require(text->textureHeight() <= text->getHeight(),
                    "translated tutorial text must fit without vertical clipping");
          }
        }
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
  enter.type = SDL_KEYDOWN;
  enter.key.keysym.sym = SDLK_RETURN;
  require(!root.handleEvents(enter) && tour->step() == NewcomerTutorialStep::Tables,
          "Enter advances the tour while consuming the input");
  tour->updateLayout(1280, 720);
  SDL_Event click{};
  click.type = SDL_MOUSEBUTTONDOWN;
  click.button.button = SDL_BUTTON_LEFT;
  click.button.x = target->getX() + 10;
  click.button.y = target->getY() + 10;
  root.handleEvents(click);
  click.type = SDL_MOUSEBUTTONUP;
  root.handleEvents(click);
  require(clicks == 0, "even the clear spotlight must block downloads and folder actions");
  tour->skip();
  click.type = SDL_MOUSEBUTTONDOWN;
  root.handleEvents(click);
  click.type = SDL_MOUSEBUTTONUP;
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
  testSkipAndInputBlocking();
  testTranslatedTipsFitAndLeaveTargetsVisible();
  testSpotlightDoesNotActivateTheUnderlyingButton();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
