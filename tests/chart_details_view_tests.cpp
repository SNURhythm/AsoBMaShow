#include "rendering/UniformCache.h"
#include "view/ChartDetailsView.h"
#include "view/ImageView.h"
#include "view/ScrollView.h"
#include "view/TextView.h"
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

std::string text(View &view, const char *name) {
  auto *label = dynamic_cast<TextView *>(view.findViewByName(name));
  require(label != nullptr, "chart details expose the requested value");
  return label->getText();
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for chart details");
  {
    View scoreColumn;
    scoreColumn.setWidth(420);
    ChartDetailsView details(new ImageView(0, 0, 0, 0));
    details.setScoreContainer(&scoreColumn);
    details.setWidth(460);
    ChartMetaRecord record;
    record.meta.Title = "Chart in the left column";
    record.meta.BmsPath = "/charts/split.bms";
    ScoreBestSnapshot best;
    best.score = 1800;
    best.maxScore = 2000;
    details.setChart(&record, best, kClearTypeHardClearRank, "300");
    details.applyYogaLayout();
    scoreColumn.applyYogaLayout();
    require(details.findViewByName("chartDetailsPersonalBest") == nullptr &&
                scoreColumn.findViewByName("chartDetailsPersonalBest") != nullptr,
            "split details keep chart facts and personal best in separate columns");
    require(text(scoreColumn, "chartDetailsScore") == "1800 / 2000",
            "the external score card receives selection updates");
    details.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    require(text(scoreColumn, "chartDetailsScore") == "—",
            "the external score card clears when the selection has no chart");
    details.setScoreContainer(nullptr);
    require(scoreColumn.getChildren().empty() &&
                text(details, "chartDetailsScore") == "—",
            "returning to landscape restores the original score card and ownership");
  }
  {
    ScrollView scroll(0, 0, 460, 800);
    auto *details = new ChartDetailsView(new ImageView(0, 0, 0, 0));
    scroll.setContentView(details);
    auto &view = *details;
    view.setWidth(460);
    ChartMetaRecord record;
    record.meta.Title = "Selected chart";
    record.meta.SubTitle = "[ANOTHER]";
    record.meta.BmsPath = "/charts/song.bms";
    record.meta.Bpm = 150;
    record.meta.MinBpm = 75.5;
    record.meta.MaxBpm = 180;
    record.meta.Rank = 2;
    record.meta.PlayLength = 125000000;
    record.meta.TotalNotes = 1350;
    record.meta.TotalLongNotes = 100;
    record.meta.TotalScratchNotes = 50;
    record.meta.HasTotal = true;
    ScoreBestSnapshot best;
    best.score = 2451;
    best.maxScore = 2700;
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    const int selectedHeight = view.getHeight();
    require(text(view, "chartDetailsBpm") == "75.5–180",
            "variable BPM preserves both ends and fractional values");
    require(text(view, "chartDetailsJudge") == "NORMAL",
            "BMS judge rank uses a human-readable name");
    require(text(view, "chartDetailsLength") == "2:05",
            "play duration converts microseconds to minutes and seconds");
    require(text(view, "chartDetailsScore") == "2451 / 2700",
            "personal best uses the cached score and its own maximum");
    require(text(view, "chartDetailsRate") == "AAA · 90.78%",
            "personal best rate and rank use the stored maximum");
    require(text(view, "chartDetailsClear") == "HARD CLEAR",
            "clear lamp is independent of the high-score attempt");
    require(text(view, "chartDetailsNext") == "MAX −249",
            "AAA progress targets MAX rather than a duplicate displayed AAA");
    // At 16:9, allow for root insets, the fixed Start / Records / Settings
    // footer and the options summary below these details. iPad 4:3 has more room.
    const int shortViewportContentHeight = 1080 - 56 - 16 - 88 - 84 - 84 - 36 - 2;
    require(view.getHeight() + 16 + 10 + 122 <= shortViewportContentHeight,
            "chart facts, best score and play settings fit above pinned actions at 16:9");

    // Selecting non-chart rows must not leave an existing scroll offset past
    // the end of the details now that placeholders preserve their height.
    scroll.setHeight(300);
    scroll.scrollToBottom();
    const float selectedOffset = scroll.getScrollOffset();
    require(selectedOffset > 0, "the details fixture starts scrolled down");
    const auto requireStableScroll = [&]() {
      view.applyYogaLayout();
      require(view.getHeight() == selectedHeight &&
                  scroll.getScrollOffset() == selectedOffset &&
                  view.getY() + view.getHeight() == scroll.getY() + scroll.getHeight(),
              "selection changes preserve the scrolled content bottom without another scroll event");
    };
    auto nonChart = record;
    nonChart.courseStart = true;
    view.setChart(&nonChart, std::nullopt, kNoClearTypeRank, "");
    requireStableScroll();
    nonChart.courseStart = false;
    nonChart.solidArchive = true;
    view.setChart(&nonChart, std::nullopt, kNoClearTypeRank, "");
    requireStableScroll();
    nonChart.solidArchive = false;
    nonChart.unavailable = true;
    view.setChart(&nonChart, std::nullopt, kNoClearTypeRank, "");
    requireStableScroll();
    view.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    requireStableScroll();
    scroll.setHeight(800);
    scroll.refreshContentLayout();

    record.meta.MinBpm = record.meta.MaxBpm = 0;
    record.meta.RankType = bms_parser::JudgeRankType::DefExRank;
    record.meta.Rank = 87;
    view.setChart(&record, std::nullopt, kNoClearTypeRank, "320");
    require(text(view, "chartDetailsBpm") == "150",
            "missing BPM range falls back to the chart's initial BPM");
    require(text(view, "chartDetailsJudge") == "87%",
            "extended judge rank is not mislabeled as an easy BMS rank");
    require(text(view, "chartDetailsScore").empty() &&
                text(view, "chartDetailsNext").empty(),
            "an unplayed selection clears previous score and progress");

    record.courseStart = true;
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    require(view.findViewByName("chartDetailsFacts")->getVisible() &&
                text(view, "chartDetailsScore") == "—",
            "course headers display placeholders instead of stale chart values");
    record.courseStart = false;
    record.solidArchive = true;
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    require(text(view, "chartDetailsBpm") == "—",
            "archive selections display placeholders instead of synthetic metadata");

    view.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    view.applyYogaLayout();
    require(text(view, "chartDetailsScore") == "—" &&
                text(view, "chartDetailsBpm") == "—",
            "clearing the selection replaces stale chart and score values with placeholders");
    require(view.getHeight() == selectedHeight,
            "empty selection reserves the same detail height as a played chart");
    record.solidArchive = false;
    view.setChart(&record, std::nullopt, kNoClearTypeRank, "320");
    view.applyYogaLayout();
    require(view.getHeight() == selectedHeight,
            "unplayed charts keep actions at the same position as played charts");
    view.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    view.applyYogaLayout();
    require(view.getHeight() == selectedHeight,
            "clearing an unplayed chart also preserves the detail height");

    record.solidArchive = false;
    record.meta.PlayLength = 0;
    best.maxScore = 0;
    view.setChart(&record, best, kClearTypeEasyClearRank, "320");
    require(text(view, "chartDetailsLength") == "—" &&
                text(view, "chartDetailsScore") == "2451 / 2700",
            "unknown length stays unknown and legacy scores use chart max as fallback");
    for (const auto mode : {ui_theme::ThemeMode::Dark, ui_theme::ThemeMode::Light}) {
      ui_theme::setActiveMode(mode);
      scroll.propagateThemeChange();
      view.applyYogaLayout();
      auto *bpm = dynamic_cast<TextView *>(view.findViewByName("chartDetailsBpm"));
      const auto expectedColor = ui_theme::textPrimary();
      const auto color = bpm->currentColor();
      require(color.r == expectedColor.r && color.g == expectedColor.g &&
                  color.b == expectedColor.b,
              "changing theme reaches details owned by the scrolling panel");
      for (const char *name : {"chartDetailsBpm", "chartDetailsJudge",
                              "chartDetailsScore", "chartDetailsClear"}) {
        auto *value = view.findViewByName(name);
        require(value->getWidth() > 0 && value->getX() >= view.getX() &&
                    value->getX() + value->getWidth() <= view.getX() + view.getWidth(),
                "chart values remain within the details panel");
      }
    }
  }
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
}
