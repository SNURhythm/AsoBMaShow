#include "rendering/UniformCache.h"
#include "view/ChartDetailsView.h"
#include "view/ImageView.h"
#include "view/ScrollView.h"
#include "view/TextView.h"
#include "view/UiTheme.h"
#include "view/MainMenuTheme.h"

#include <bgfx/bgfx.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
class ArtworkLayoutDetails : public ChartDetailsView {
public:
  using ChartDetailsView::ChartDetailsView;
  using ChartDetailsView::renderImpl;
};

void writePpm(const std::filesystem::path &path, int width, int height) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "P6\n" << width << ' ' << height << "\n255\n";
  const char pixel[] = {char(0x33), char(0x66), char(0x99)};
  for (int index = 0; index < width * height; ++index) output.write(pixel, sizeof(pixel));
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for chart details");
  {
    const auto fixture = std::filesystem::temp_directory_path() /
        ("asobmashow-details-" + std::to_string(SDL_GetTicksNS()) + ".ppm");
    auto *image = new ImageView(0, 0, 0, 0);
    ArtworkLayoutDetails details(image);
    details.setWidth(472);
    details.setCompact(false);
    ChartMetaRecord record;
    record.meta.BmsPath = "/charts/artwork.bms";
    record.meta.StageFile = fixture.filename().string();
    details.setChart(&record, std::nullopt, kNoClearTypeRank, "");
    auto *frame = details.findViewByName("chartDetailsArtwork");
    auto *summary = details.findViewByName("chartDetailsSummary");
    details.applyYogaLayout();
    require(frame->getWidth() == 472 && frame->getHeight() == 472,
            "missing artwork uses a square placeholder");
    const int placeholderSummaryY = summary->getY();
    writePpm(fixture, 32, 18);
    require(image->setImage(fspath_to_path_t(fixture)), "wide stagefile fixture decodes");
    RenderContext context;
    details.renderImpl(context);
    details.applyYogaLayout();
    require(frame->getWidth() == 472 && frame->getHeight() == 472 &&
                image->getWidth() == frame->getWidth() && image->getHeight() == frame->getHeight() &&
                summary->getY() == placeholderSummaryY,
            "decoded wide artwork retains the square frame and stable metadata position");
    const int wideHeight = details.getHeight();
    details.renderImpl(context);
    require(details.getHeight() == wideHeight && !YGNodeIsDirty(details.getNode()),
            "unchanged decoded dimensions do not dirty layout on another frame");
    details.setCompact(true);
    details.applyYogaLayout();
    require(frame->getWidth() == 112 && frame->getHeight() == 112,
            "portrait contains wide artwork in a square frame");
    image->freeImage();
    ImageView::dropCache(fspath_to_path_t(fixture));
    writePpm(fixture, 18, 32);
    require(image->setImage(fspath_to_path_t(fixture)), "tall stagefile fixture decodes");
    details.renderImpl(context);
    details.applyYogaLayout();
    require(frame->getWidth() == 112 && frame->getHeight() == 112,
            "tall artwork retains the square compact frame");
    record.meta.StageFile.clear();
    details.setChart(&record, std::nullopt, kNoClearTypeRank, "");
    details.applyYogaLayout();
    require(frame->getWidth() == 112 && frame->getHeight() == 112,
            "missing artwork retains the square placeholder after a tall image");
    std::filesystem::remove(fixture);
  }
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
    const int naturalHeight = details.getHeight();
    details.setMinHeight(520);
    details.applyYogaLayout();
    const auto *facts = details.findViewByName("chartDetailsFacts");
    const auto *lastValue = details.findViewByName("chartDetailsTotal");
    require(details.getHeight() == 520 &&
                facts->getY() + facts->getHeight() == details.getY() + details.getHeight() &&
                std::abs(lastValue->getY() + lastValue->getHeight() -
                         (facts->getY() + facts->getHeight() - 14)) <= 1,
            "portrait chart facts distribute content through the aligned column height");
    for (const int height : {520, 720}) {
      details.setMinHeight(height);
      details.applyYogaLayout();
      auto *bpm = details.findViewByName("chartDetailsBpm");
      auto *length = details.findViewByName("chartDetailsLength");
      auto *bpmCell = static_cast<View *>(YGNodeGetContext(YGNodeGetParent(bpm->getNode())));
      auto *lengthCell = static_cast<View *>(YGNodeGetContext(YGNodeGetParent(length->getNode())));
      auto *bpmLabel = bpmCell->getChildren().front();
      auto *lengthLabel = lengthCell->getChildren().front();
      const int pairGap = bpm->getY() - bpmLabel->getY() - bpmLabel->getHeight();
      const int rowGap = lengthLabel->getY() - bpm->getY() - bpm->getHeight();
      require(pairGap == 0 && rowGap >= 8 && rowGap > pairGap,
              "stretched portrait facts keep label/value pairs close and distribute space between rows");
    }
    record.meta.Title = "A long portrait chart title that must scroll horizontally rather than wrap into clipped lines";
    details.setChart(&record, best, kClearTypeHardClearRank, "300");
    details.applyYogaLayout();
    auto *portraitTitle = dynamic_cast<TextView *>(details.findViewByName("chartDetailsTitle"));
    require(portraitTitle->textureWidth() > portraitTitle->getWidth() &&
                portraitTitle->textureHeight() <= portraitTitle->getHeight(),
            "portrait long titles retain a single overflowing line for marquee");
    details.setMinHeight(0);
    details.applyYogaLayout();
    require(details.getHeight() == naturalHeight,
            "removing the portrait fill constraint restores natural chart height");
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
    auto *artworkPlaceholder = view.findViewByName("chartDetailsArtworkPlaceholder");
    auto *reservedArtwork = view.findViewByName("chartDetailsArtwork");
    require(artworkPlaceholder && artworkPlaceholder->getVisible() &&
                reservedArtwork && reservedArtwork->getVisible() &&
                reservedArtwork->getWidth() > 0 && reservedArtwork->getHeight() > 0,
            "a chart without a stage image retains its artwork frame and music placeholder");
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
    record.meta.KeyMode = 7;
    record.meta.PlayLevel = 11;
    for (const std::string units : {"★11", "sl3", "st2", "◆発狂", "sl3 / ★11"}) {
      record.difficultyTableLabels = units;
      view.setChart(&record, best, kClearTypeHardClearRank, "320");
      require(text(view, "chartDetailsDifficulty") == units,
              "table difficulty is the primary label and preserves every unit verbatim");
      require(text(view, "chartDetailsChartLevel") == "7K1S · Lv. 11",
              "the chart's own level remains available separately from table difficulty");
    }
    record.difficultyTableLabels.clear();
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    require(text(view, "chartDetailsDifficulty") == "Lv. 11" &&
                text(view, "chartDetailsChartLevel") == "7K1S",
            "charts outside a difficulty table fall back to their own level without duplicate labels");
    // At 16:9, the play dock is separate from the details and records column.
    const int shortViewportContentHeight = 1080 - 56 - 24 - 104 - 84 - 12;
    require(view.getHeight() + 32 <= shortViewportContentHeight,
            "chart facts and best score fit above pinned records at 16:9");

    // Landscape artwork uses a square frame; long details remain scrollable
    // without moving the primary controls into that scroll container.
    record.meta.StageFile = "stage.png";
    view.setCompact(false);
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(view.getHeight() > selectedHeight, "landscape artwork reserves its own space");
    auto *badge = dynamic_cast<TextView *>(view.findViewByName("chartDetailsDifficulty"));
    const int badgeLeft = badge->getX();
    const int shortBadgeWidth = badge->getWidth();
    record.difficultyTableLabels = "sl3 / ★11";
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(badge->getX() == badgeLeft && badge->getWidth() > shortBadgeWidth &&
                badge->getWidth() >= badge->measureTextWidth(badge->getText()) + 24,
            "detail level badge grows right to fit text with horizontal insets");
    record.difficultyTableLabels = "sl3 / ★11 / st2 / ◆15 / sl12 / ★25";
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    auto *chartLevelValue = view.findViewByName("chartDetailsChartLevel");
    require(badge->getX() == badgeLeft &&
                badge->getX() + badge->getWidth() <= chartLevelValue->getX() &&
                chartLevelValue->getWidth() >= 96 &&
                chartLevelValue->getX() + chartLevelValue->getWidth() <= view.findViewByName("chartDetailsGrade")->getX(),
            "long badges preserve space for key mode and the best-grade column");
    record.difficultyTableLabels.clear();
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(badge->getWidth() == shortBadgeWidth,
            "short levels restore their natural badge width");
    auto *artwork = view.findViewByName("chartDetailsArtwork");
    auto *summary = view.findViewByName("chartDetailsSummary");
    require(artwork && artwork->getWidth() == 460 && artwork->getHeight() == 460 &&
                summary && summary->getY() > artwork->getY() + artwork->getHeight(),
            "desktop artwork occupies the hero frame above the chart and best-score summary");
    require(view.findViewByName("chartDetailsTotal")->getX() < view.findViewByName("chartDetailsNotes")->getX() &&
                view.findViewByName("chartDetailsNotes")->getX() < view.findViewByName("chartDetailsNoteTypes")->getX() &&
                view.findViewByName("chartDetailsNoteTypes")->getX() < view.findViewByName("chartDetailsJudge")->getX(),
            "desktop facts contain total, notes, note types and judge without duplicated tempo");
    require(YGNodeStyleGetDisplay(YGNodeGetParent(view.findViewByName("chartDetailsBpm")->getNode())) == YGDisplayNone &&
                YGNodeStyleGetDisplay(YGNodeGetParent(view.findViewByName("chartDetailsLength")->getNode())) == YGDisplayNone,
            "BPM and length occur only in the desktop summary");
    require(view.findViewByName("chartDetailsGenre")->getY() >=
                view.findViewByName("chartDetailsArtist")->getY() + view.findViewByName("chartDetailsArtist")->getHeight() &&
                view.findViewByName("chartDetailsGenre")->getY() < summary->getY() &&
                view.findViewByName("chartDetailsMore") == nullptr,
            "genre follows the artist and the title has no overflow action");
    require(view.findViewByName("chartDetailsDifficulty")->getY() < view.findViewByName("chartDetailsTempo")->getY() &&
                view.findViewByName("chartDetailsGrade")->getX() > view.findViewByName("chartDetailsChartLevel")->getX(),
            "desktop summary places tempo below difficulty and the best grade on the right");
    require(text(view, "chartDetailsBestRate") == "90.78%" &&
                text(view, "chartDetailsTempo") == "BPM 75.5–180   |   2:05",
            "desktop summary displays percentage without repeating the grade and preserves tempo range");
    require(text(view, "chartDetailsJudge") == "NORMAL" && text(view, "chartDetailsScore") == "2451 / 2700",
            "all chart metadata and cached scores are visible without unfolding");
    auto *scoreCard = view.findViewByName("chartDetailsPersonalBest");
    auto *facts = view.findViewByName("chartDetailsFacts");
    require(scoreCard->getY() >= summary->getY() + summary->getHeight() &&
                scoreCard->getY() + scoreCard->getHeight() <= facts->getY() &&
                !view.findViewByName("chartDetailsRate")->getVisible() &&
                view.findViewByName("chartDetailsNext")->getVisible(),
            "desktop EX score and next-rank card sits below the summary without duplicate grade/rate");
    auto *next = view.findViewByName("chartDetailsNext");
    auto *clear = dynamic_cast<TextView *>(view.findViewByName("chartDetailsClear"));
    auto *scoreValue = view.findViewByName("chartDetailsScore");
    require(next->getX() + next->getWidth() == scoreCard->getX() + scoreCard->getWidth() &&
                clear->getX() + clear->getWidth() == scoreCard->getX() + scoreCard->getWidth() &&
                next->getY() < clear->getY() && clear->getY() == scoreValue->getY() &&
                clear->pointSize() == 32 && clear->fontWeight() == TextView::FontWeight::Bold,
            "next rank occupies the upper right above the larger clear rank at lower right");
    require(clear->getHeight() == scoreValue->getHeight() && clear->getWidth() > 100 &&
                clear->currentColor().r == 0 && clear->currentColor().g == 0 &&
                clear->currentColor().b == 0 && !view.findViewByName("chartDetailsClearLamp"),
            "clear status fills its score-row rectangle with contrasting text and no thin lamp");
    const int fullHeight = view.getHeight();
    record.meta.StageFile.clear();
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(artwork->getVisible() && artworkPlaceholder->getVisible() &&
                view.getHeight() == fullHeight,
            "missing landscape artwork preserves the hero frame and panel height");
    view.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    view.applyYogaLayout();
    require(artwork->getVisible() && artworkPlaceholder->getVisible() &&
                view.getHeight() == fullHeight,
            "an empty landscape selection retains the same artwork placeholder");
    record.meta.StageFile = "stage.png";
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(text(view, "chartDetailsGrade") == "AAA",
            "desktop summary gives the cached best grade its own display");

    scroll.setHeight(shortViewportContentHeight);
    scroll.refreshContentLayout();
    scroll.scrollToBottom();
    auto *score = view.findViewByName("chartDetailsScore");
    require(score->getY() >= scroll.getY() &&
                score->getY() + score->getHeight() <= scroll.getY() + scroll.getHeight(),
            "personal best remains reachable with landscape artwork");
    record.meta.StageFile.clear();
    view.setCompact(true);
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();

    require(view.findViewByName("chartDetailsRate")->getVisible(),
            "portrait score card retains its sole rank/rate display after rotation");
    require(view.findViewByName("chartDetailsBpm")->getX() < view.findViewByName("chartDetailsJudge")->getX() &&
                view.findViewByName("chartDetailsLength")->getX() < view.findViewByName("chartDetailsNotes")->getX(),
            "portrait rotation restores the original metric order in two columns");
    require(view.findViewByName("chartDetailsBpm")->getY() < view.findViewByName("chartDetailsLength")->getY() &&
                view.findViewByName("chartDetailsLength")->getY() < view.findViewByName("chartDetailsTotal")->getY(),
            "portrait rotation restores the original vertical metric row order");

    // Selecting non-chart rows must not leave an existing scroll offset past
    // the end of the details now that placeholders preserve their height.
    scroll.setHeight(300);
    scroll.scrollToBottom();
    const float selectedOffset = scroll.getScrollOffset();
    require(selectedOffset > 0, "the details fixture starts scrolled down");
    const auto requireStableScroll = [&]() {
      view.applyYogaLayout();
      require(reservedArtwork->getVisible() && artworkPlaceholder->getVisible(),
              "non-chart selections retain the artwork placeholder");
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
      const auto expectedColor = main_menu_theme::ink();
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
