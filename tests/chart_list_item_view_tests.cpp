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

    ChartListItemView row(0, 0, 964, 108, record);
    row.setMeta(record);
    row.applyYogaLayout();

    auto *card = row.findViewByName("chartListContentCard");
    auto *banner = dynamic_cast<ImageView *>(
        row.findViewByName("chartListBanner"));
    auto *difficulty = dynamic_cast<TextView *>(
        row.findViewByName("chartListDifficulty"));
    auto *keyMode = dynamic_cast<TextView *>(
        row.findViewByName("chartListKeyMode"));
    require(card != nullptr && banner != nullptr,
            "chart row exposes its card and banner background");
    require(difficulty != nullptr && keyMode != nullptr,
            "chart row exposes difficulty and key-mode labels");
    require(difficulty->fontWeight() == TextView::FontWeight::Bold &&
                keyMode->fontWeight() == TextView::FontWeight::Bold,
            "difficulty and key-mode labels use bold text");
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

    for (const int keys : {5, 7}) {
      record.meta.KeyMode = keys;
      for (const auto [scratch, backspin] :
           {std::pair{0, 0}, std::pair{1, 0}, std::pair{0, 1}}) {
        record.meta.TotalScratchNotes = scratch;
        record.meta.TotalBackSpinNotes = backspin;
        row.setMeta(record);
        const auto expected = std::to_string(keys) +
                              (scratch || backspin ? "K1S" : "K");
        require(keyMode->getText() == expected,
                "chart list distinguishes scratchless charts and long scratches");
        require(record.meta.KeyMode == keys,
                "display labels preserve the canonical chart identity");
      }
    }
    record.meta.TotalScratchNotes = 0;
    record.meta.TotalBackSpinNotes = 0;
    for (const auto [keys, label] : {std::pair{10, "5KDP"}, std::pair{14, "7KDP"},
                                    std::pair{9, "9K"}}) {
      record.meta.KeyMode = keys;
      row.setMeta(record);
      require(keyMode->getText() == label, "DP and non-beat labels remain intact");
    }

    row.setBestScoreRank(2451, 2700);
    row.applyYogaLayout();
    auto *bestScore = dynamic_cast<TextView *>(
        row.findViewByName("chartListBestScore"));
    require(bestScore != nullptr && bestScore->getText() == "90.78%",
            "played rows display the best EX rate inside the rank badge");
    auto *rankBadge = row.findViewByName("chartListScoreRank");
    require(rankBadge != nullptr && rankBadge->getVisible() &&
                bestScore->getX() >= rankBadge->getX() &&
                bestScore->getY() >= rankBadge->getY() &&
                bestScore->getY() + bestScore->getHeight() <=
                    rankBadge->getY() + rankBadge->getHeight(),
            "best score fits within the existing rank badge");
    row.setBestScoreRank(12345, 15000);
    require(bestScore->getText() == "82.30%",
            "rebinding updates the EX rate as well as the grade");
    row.setBestScoreRank(15000, 15000);
    require(bestScore->getText() == "100.00%",
            "a perfect score displays a full EX rate");
    require(bestScore->measureTextWidth(bestScore->getText()) <= bestScore->getWidth(),
            "100.00% fits without clipping in the rank badge");
    row.setBestScoreRank(0, 2700);
    require(rankBadge->getVisible() && bestScore->getText() == "0.00%",
            "a recorded zero EX score remains visible as a zero rate");
    bool hasFailingGrade = false;
    for (auto *child : rankBadge->getChildren()) {
      if (auto *text = dynamic_cast<TextView *>(child)) {
        hasFailingGrade = hasFailingGrade || text->getText() == "F";
      }
    }
    require(hasFailingGrade, "a recorded zero EX score displays grade F");
    row.setBestScoreRank(0, 0);
    require(!rankBadge->getVisible() && bestScore->getText().empty(),
            "unplayed rows hide the badge and clear the previous score");
    row.setBestScoreRank(2451, 2700);

    record.meta.Banner.clear();
    row.setMeta(record);
    require(banner->imagePath().empty(),
            "rebind without a banner clears recycled image identity");
    require(!rankBadge->getVisible() && bestScore->getText().empty(),
            "recycling a row clears the score before a new best is bound");

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
