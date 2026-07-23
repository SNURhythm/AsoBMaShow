#include "rendering/UniformCache.h"
#include "view/ChartListItemView.h"
#include "view/ImageView.h"
#include "view/UiTheme.h"

#include <bgfx/bgfx.h>

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
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool sameColor(const Color &actual, const Color &expected) {
  return actual.r == expected.r && actual.g == expected.g &&
         actual.b == expected.b && actual.a == expected.a;
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for chart list row");

  {
    ui_theme::setActiveMode(ui_theme::ThemeMode::Dark);
    ChartMetaRecord record;
    record.meta.Folder = "/charts/first";
    record.meta.Banner = "banner.png";
    record.meta.Title = "Banner Song";
    record.meta.Artist = "Banner Artist";

    ChartListItemView row(0, 0, 1200, 108, record);
    row.setMeta(record);
    row.applyYogaLayout();

    auto *card = row.findViewByName("chartListContentCard");
    auto *banner = dynamic_cast<ImageView *>(
        row.findViewByName("chartListBanner"));
    require(card != nullptr && banner != nullptr,
            "chart row exposes its card and banner background");
    require(banner->imagePath() == path_t("/charts/first/banner.png"),
            "chart row binds Folder/Banner through ImageView");
    require(banner->fade().has_value() &&
                banner->fade()->direction ==
                    ImageFadeDirection::LeftToRight &&
                banner->fade()->strength == 1.0F,
            "chart banner fades in from left to right at full strength");
    require(banner->scrimColor().has_value() &&
                sameColor(*banner->scrimColor(), Color(5, 10, 18, 144)),
            "dark chart banner uses the readability scrim");
    ui_theme::setActiveMode(ui_theme::ThemeMode::Light);
    row.propagateThemeChange();
    require(banner->scrimColor().has_value() &&
                sameColor(*banner->scrimColor(),
                          Color(255, 255, 255, 168)),
            "chart banner scrim follows the active light theme");
    ui_theme::setActiveMode(ui_theme::ThemeMode::Dark);
    row.propagateThemeChange();
    require(banner->getWidth() == 368 &&
                banner->getX() + banner->getWidth() ==
                    card->getX() + card->getWidth() - 1 &&
                banner->getY() == card->getY() + 1,
            "chart banner is inset and anchored to the card right edge");
    require(!card->getChildren().empty() &&
                card->getChildren().front() == banner,
            "chart banner renders behind row content");

    record.meta.Banner.clear();
    row.setMeta(record);
    require(banner->imagePath().empty(),
            "rebind without a banner clears recycled image identity");

    record.meta.Banner = "banner.png";
    record.unavailable = true;
    row.setMeta(record);
    require(banner->imagePath().empty(),
            "unavailable rows do not request banner files");

    record.unavailable = false;
    record.solidArchive = true;
    row.setMeta(record);
    require(banner->imagePath().empty(),
            "solid archive rows do not request banner files");

    record.solidArchive = false;
    record.courseStart = true;
    row.setMeta(record);
    require(banner->imagePath().empty(),
            "course rows do not borrow a chart banner");
  }

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
