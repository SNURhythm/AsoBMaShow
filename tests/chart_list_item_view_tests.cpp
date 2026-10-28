#include "rendering/UniformCache.h"
#include "view/ChartListItemView.h"
#include "view/GoalTargetBubble.h"
#include "view/GoalLampTabAccent.h"
#include "view/Button.h"
#include "view/LibraryFolderItemView.h"
#include "view/ImageView.h"
#include "view/UiTheme.h"
#include "view/MainMenuTheme.h"
#include "view/IconText.h"
#include "i18n/Localization.h"

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

void testGoalLampAccentDrawsAfterParentLayoutAndScroll() {
  struct Backend final : rendering::UiBatchBackend {
    std::vector<rendering::UiBatchState> draws;
    bool submit(const rendering::UiBatchSubmission &submission) noexcept override {
      draws.push_back(submission.state);
      return true;
    }
  } backend;
  rendering::UiBatchRenderer batch(backend);
  RenderContext context(batch);
  View root(30, 40, 300, 180);
  auto *tab = new Button();
  tab->setWidth(240)->setHeight(52)->setFlexShrink(0);
  auto *accent = new GoalLampTabAccent(kClearTypeFullComboRank);
  tab->addView(accent);
  root.addView(tab);
  for (int y : {40, 10, 70}) {
    root.setPositionNoLayout(30, y);
    root.applyYogaLayout();
    backend.draws.clear();
    batch.begin();
    accent->render(context);
    batch.flush();
    require(backend.draws.size() == 1,
            "Full Combo tab draws its lamp after parent layout and scrolling");
    const auto &clip = backend.draws.front().scissor;
    require(clip && clip->x == accent->getX() && clip->y == accent->getY() &&
                clip->width == 7 && clip->height == 52,
            "lamp clip follows the tab's current position and height");
  }
}

void testFolderBindingOnlyMaterializesVisibleLabel() {
  LibraryFolderItemView row(0, 0, 350, 50);
  std::vector<TextView *> labels;
  for (auto *child : row.getChildren().front()->getChildren()) {
    if (auto *text = dynamic_cast<TextView *>(child)) labels.push_back(text);
  }
  for (int index = 0; index < 9; ++index) {
    const bool section = index % 3 == 2;
    const int depth = index % 3 == 1 ? 1 : 0;
    const bool selected = index % 2 != 0;
    std::vector<bgfx::TextureHandle> previous;
    for (auto *label : labels) previous.push_back(label->textureHandle());
    const std::string value = "Recycled folder " + std::to_string(index);
    {
      View::LayoutBatchScope batch;
      row.setItem(value, depth, -1, selected, kNoClearTypeRank,
                  false, false, false, 0, section);
    }
    int displayedLabels = 0;
    for (std::size_t i = 0; i < labels.size(); ++i) {
      auto *label = labels[i];
      const auto texture = label->textureHandle();
      if (!label->getVisible() || YGNodeStyleGetDisplay(label->getNode()) == YGDisplayNone) {
        require(!bgfx::isValid(texture) || texture.idx == previous[i].idx,
                "recycled folder binding must not upload a hidden label variant");
      } else if (label->getText() == value) {
        ++displayedLabels;
        require(bgfx::isValid(texture), "the active folder label has a raster");
        const auto expected = section ? main_menu_theme::muted()
            : selected ? main_menu_theme::onAction() : main_menu_theme::ink();
        const auto actual = label->currentColor();
        require(actual.r == expected.r && actual.g == expected.g &&
                    actual.b == expected.b && actual.a == expected.a,
                "recycling folder variants preserves selected and section colors");
      }
    }
    require(displayedLabels == 1, "recycling root, leaf and section rows binds exactly one current label");
  }
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for chart list row");

  for (const auto [x, y] : {std::pair{0, 0}, std::pair{310, 0}, std::pair{0, 190}, std::pair{310, 190}}) {
    const auto p = placeGoalBubble(x, y, 36, 48, 320, 200,
                                   {.top=12, .right=14, .bottom=10, .left=16});
    require(p.x >= 20 && p.y >= 16 && p.x + p.width <= 302 && p.y + p.height <= 186,
            "goal bubble stays inside every safe-area edge");
  }
  const auto aboveFlag = placeGoalBubble(160, 100, 40, 40, 400, 300, {});
  require(aboveFlag.x + aboveFlag.width / 2 == 180 && aboveFlag.y + aboveFlag.height == 96,
          "goal bubble is centered above the flag with a four-pixel gap");
  testFolderBindingOnlyMaterializesVisibleLabel();
  testGoalLampAccentDrawsAfterParentLayoutAndScroll();

  {
    ui_theme::setActiveMode(ui_theme::ThemeMode::Dark);
    ChartMetaRecord record;
    record.meta.Folder = "/charts/first";
    record.meta.BmsPath = "/charts/first/song.bms";
    record.meta.Banner = "banner.png";
    record.meta.Title = "Banner Song";
    record.meta.Artist = "Banner Artist";

    ChartListItemView row(0, 0, 964, 108, record);
    row.setMeta(record);
    row.applyYogaLayout();

    auto *card = row.findViewByName("chartListContentCard");
    auto *jacket = dynamic_cast<ImageView *>(row.findViewByName("chartListJacket"));
    auto *difficulty = dynamic_cast<TextView *>(row.findViewByName("chartListDifficulty"));
    auto *keyMode = dynamic_cast<TextView *>(row.findViewByName("chartListKeyMode"));
    auto *title = row.findViewByName("chartListTitle");
    auto *artist = row.findViewByName("chartListArtist");
    auto *artwork = row.findViewByName("chartListArtwork");
    require(card && jacket && difficulty && keyMode && title && artist && artwork,
            "the row exposes its aligned data columns and artwork");
    require(difficulty->fontWeight() == TextView::FontWeight::Bold &&
                keyMode->fontWeight() == TextView::FontWeight::Bold,
            "table difficulty and key mode retain clear typographic hierarchy");
    require(jacket->imagePath() == path_t("/charts/first/banner.png"),
            "banner artwork is a thumbnail fallback when a stage image is absent");
    require(artwork->getWidth() == 72 && artwork->getHeight() == 72 &&
                artist->getX() == title->getX() &&
                artist->getY() >= title->getY() + title->getHeight() &&
                difficulty->getX() >= title->getX() + title->getWidth(),
            "desktop rows stack title and artist beside the thumbnail without overlapping difficulty");
    auto *leftLamp = row.findViewByName("chartListClearLamp");
    require(leftLamp && leftLamp->getX() < artwork->getX() &&
                leftLamp->getHeight() > leftLamp->getWidth(),
            "the clear lamp is a compact vertical bar before the artwork");
    record.meta.StageFile = "stage.png";
    row.setMeta(record);
    require(jacket->imagePath() == path_t("/charts/first/stage.png"),
            "stage artwork takes priority over a banner");
    record.meta.StageFile.clear();
    for (const auto mode : {ui_theme::ThemeMode::Light, ui_theme::ThemeMode::Dark}) {
      ui_theme::setActiveMode(mode);
      row.propagateThemeChange();
      record.difficultyTableLabels = "sl3 / ★11";
      row.setMeta(record);
      row.setClearRank(kClearTypeHardClearRank);
      row.onSelected();
      auto *lamp = row.findViewByName("chartListClearLamp");
      require(lamp && YGNodeStyleGetBorder(lamp->getNode(), YGEdgeAll) == 0,
              "clear lamps have no hard border on the selected surface");
      const auto color = difficulty->currentColor();
      const auto expected = main_menu_theme::onAction();
      require(difficulty->getText() == record.difficultyTableLabels &&
                  color.r == expected.r && color.g == expected.g && color.b == expected.b,
              "selected table units stay verbatim and readable in both themes");
      row.onUnselected();
      require(jacket->getVisible(), "selection retains the dedicated thumbnail");
    }
    record.difficultyTableLabels.clear();
    row.setMeta(record);
    row.applyYogaLayout();
    const int shortLevelWidth = difficulty->getWidth();
    const int difficultyRight = difficulty->getX() + difficulty->getWidth();
    const int shortTitleWidth = title->getWidth();
    record.difficultyTableLabels = "sl12 / ★25 / st10";
    row.setMeta(record);
    row.applyYogaLayout();
    require(difficulty->getWidth() >= difficulty->measureTextWidth(difficulty->getText()) &&
                difficulty->getWidth() > shortLevelWidth &&
                difficulty->getX() + difficulty->getWidth() == difficultyRight &&
                keyMode->getX() + keyMode->getWidth() == difficultyRight &&
                title->getWidth() < shortTitleWidth,
            "long difficulty units expand left without clipping or moving the right-aligned key mode");
    record.difficultyTableLabels.clear();
    row.setMeta(record);
    row.applyYogaLayout();
    require(difficulty->getWidth() == shortLevelWidth && title->getWidth() == shortTitleWidth,
            "recycled short levels return space to the title column");

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
    require(rankBadge->getVisible() && bestScore->getText().empty(),
            "unplayed rows keep column alignment and clear the previous score");
    row.setBestScoreRank(2451, 2700);

    ChartListItemView initiallyNarrow(0, 0, 620, 100, record);
    initiallyNarrow.setMeta(record);
    initiallyNarrow.applyYogaLayout();
    row.setWidth(620);
    row.applyYogaLayout();
    require(artist->getY() >= title->getY() + title->getHeight() &&
                difficulty->getX() >= title->getX() + title->getWidth(),
            "narrow rows put the artist under the title without obscuring difficulty");
    require(row.findViewByName("chartListClearLamp")->getWidth() == 6,
            "compact rows preserve a visible clear lamp");
    row.setWidth(964);
    row.applyYogaLayout();
    require(artist->getX() == title->getX() &&
                artist->getY() >= title->getY() + title->getHeight(),
            "widening preserves the stacked title and artist layout");
    require(leftLamp->getX() < artwork->getX() &&
                leftLamp->getHeight() > leftLamp->getWidth(),
            "rotation keeps the clear lamp at the leading edge");
    record.difficultyTableLabels = "sl12 / ★25 / st10 / ◆12 / sl15 / ★24 / st11 / sl13 / ★22 / st9 / ◆15";
    row.setMeta(record);
    row.applyYogaLayout();
    const int wideDifficultyWidth = difficulty->getWidth();
    auto *goal = row.findViewByName("chartListGoal");
    require(goal && goal->getVisible(), "playable rows retain their goal action");
    std::string openedPath;
    row.setGoalHandler([&](const ChartMetaRecord &clicked) { openedPath = clicked.meta.BmsPath; });
    row.setGoalState(true);
    row.setGoalTarget(400, "HARD", "AAA");
    auto *bubble = row.findViewByName("chartGoalBubble");
    auto *bar = row.findViewByName("chartGoalBubbleLampBar");
    auto *lampText = dynamic_cast<TextView *>(row.findViewByName("chartGoalBubbleLamp"));
    auto *scoreText = dynamic_cast<TextView *>(row.findViewByName("chartGoalBubbleScore"));
    require(bubble && bar && lampText && scoreText && !bubble->getVisible(),
            "unselected goals do not show the target bubble");
    row.onSelected();
    require(bubble->getVisible() && bar->getVisible() && lampText->getText() == "HARD" &&
                scoreText->getText() == "AAA", "selected unfinished goal shows lamp bar and targets");
    row.applyYogaLayout();
    const int lampCenter = (bar->getX() + lampText->getX() + lampText->getWidth()) / 2;
    const int scoreCenter = scoreText->getX() + scoreText->getWidth() / 2;
    require(std::abs(lampCenter - scoreCenter) <= 1,
            "lamp bar and label are centered together above the centered score");
    row.setGoalTarget(-1, "", "99.99%");
    require(bubble->getVisible() && !bar->getVisible() && scoreText->getText() == "99.99%",
            "score-only goals omit the lamp bar and show the custom rate");
    row.setGoalTarget(400, "HARD", "AAA");
    row.setGoalState(true, true);
    require(!bubble->getVisible(), "achieved goals hide the bubble");
    row.setGoalState(true);
    require(bubble->getVisible(), "unfinished goal restores the bubble");
    row.onUnselected();
    require(!bubble->getVisible(), "leaving the chart hides the bubble");
    row.setGoalState(true, true);
    auto *flag = dynamic_cast<TextView *>(dynamic_cast<Button *>(goal)->getContentView()->findViewByName("chartListGoalFlag"));
    require(flag != nullptr, "goal button contains its flag icon");
    const auto achievedColor = flag->currentColor();
    row.setGoalState(true);
    const auto activeColor = flag->currentColor();
    require(achievedColor.r != activeColor.r || achievedColor.g != activeColor.g ||
                achievedColor.b != activeColor.b, "achieved flags use a distinct color");
    require(dynamic_cast<Button *>(goal)->isSelected(), "configured goals highlight their flag");
    SDL_Event click{};
    click.button.button = SDL_BUTTON_LEFT;
    click.button.x = goal->getX() + goal->getWidth() / 2;
    click.button.y = goal->getY() + goal->getHeight() / 2;
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN; row.handleEvents(click);
    click.type = SDL_EVENT_MOUSE_BUTTON_UP; row.handleEvents(click);
    require(openedPath == record.meta.BmsPath, "flag taps open the goal for that row's chart");
    row.setMeta(record);
    row.onSelected();
    require(!bubble->getVisible(), "recycled rows clear the old goal bubble");
    require(!dynamic_cast<Button *>(goal)->isSelected(), "recycled rows clear stale goal highlights");
    for (const int width : {620, 420, 780, 620}) {
      row.setWidth(width);
      row.applyYogaLayout();
      require(difficulty->getX() >= title->getX() + title->getWidth() && title->getWidth() >= 80 &&
                  rankBadge->getX() >= difficulty->getX() + difficulty->getWidth() &&
                  goal->getX() >= rankBadge->getX() + rankBadge->getWidth() &&
                  goal->getX() + goal->getWidth() <= row.getX() + row.getWidth(),
              "long multi-table labels preserve title space and keep score and goal inside narrow rows");
      require(difficulty->getWidth() < wideDifficultyWidth,
              "difficulty width adapts to resizing within the compact breakpoint");
      require(difficulty->getText() == record.difficultyTableLabels,
              "width caps retain the full difficulty label for marquee scrolling");
    }
    row.setWidth(964);
    row.applyYogaLayout();
    require(difficulty->getWidth() == wideDifficultyWidth,
            "widening restores the desired difficulty width without rebinding metadata");
    record.difficultyTableLabels.clear();
    record.meta.Banner.clear();
    row.setMeta(record);
    require(jacket->imagePath().empty(),
            "rebind without a banner clears recycled image identity");
    require(rankBadge->getVisible() && bestScore->getText().empty(),
            "recycling a row clears the score before a new best is bound");

    record.meta.Banner = "banner.png";
    record.unavailable = true;
    row.setMeta(record);
    require(jacket->imagePath().empty(),
            "unavailable rows do not request banner files");

    record.unavailable = false;
    record.solidArchive = true;
    row.setMeta(record);
    require(jacket->imagePath().empty(),
            "solid archive rows do not request banner files");

    record.solidArchive = false;
    record.courseStart = true;
    row.setMeta(record);
    require(jacket->imagePath().empty(),
            "course rows do not borrow a chart banner");
    record.courseStart = false;
    record.randomSelect = ChartSelectorRandomFilter::Any;
    row.setMeta(record);
    require(jacket->imagePath().empty() && !goal->getVisible() &&
                !rankBadge->getVisible(),
            "random rows do not borrow chart artwork, scores, or goal actions");
    ChartMetaRecord action;
    action.addRecommendedGoals = true;
    row.setMeta(action);
    row.applyYogaLayout();
    require(!artist->getVisible() && std::abs((title->getY() + title->getHeight() / 2.0F) -
                (card->getY() + card->getHeight() / 2.0F)) <= 1,
            "recommendations title is vertically centered without an empty artist row");
    auto *placeholder = dynamic_cast<TextView *>(row.findViewByName("chartListArtworkPlaceholder"));
    auto *actionTitle = dynamic_cast<TextView *>(title);
    require(placeholder && actionTitle && placeholder->getText() == ui_icons::textForCodepoint(ui_icons::kPlus) &&
                actionTitle->getText() == i18n::tr("goals.ui.add_recommended") &&
                actionTitle->fontWeight() == TextView::FontWeight::Bold && !goal->getVisible() && !rankBadge->getVisible(),
            "recommendations row reuses the chart title and artwork slot without chart controls");
    record.randomSelect.reset();
    row.setMeta(record);
    require(goal->getVisible() && rankBadge->getVisible() && artist->getVisible(),
            "recycling a random row restores ordinary chart controls");
  }

  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
