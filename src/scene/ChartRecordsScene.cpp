#include "ChartRecordsScene.h"

#include "SceneManager.h"
#include "../ResultRecordSummary.h"
#include "../rendering/common.h"
#include "../replay/ReplayFileActionService.h"
#include "../repositories/LegacyResultSummary.h"
#include "../view/Button.h"
#include "../view/ResultRecordListView.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";

TextView *makeText(std::string value, int size,
                   View::ThemeColorProvider color) {
  auto *result = new TextView(kFontPath, size);
  result->setText(std::move(value));
  result->setThemedColor(std::move(color));
  result->setVAlign(TextView::MIDDLE);
  result->setOverflow(TextView::TextOverflow::Hidden);
  return result;
}

Button *makeButton(std::string label) {
  auto *button = new Button();
  button->setWidth(116)->setHeight(52)->setCornerRadius(
      ui_theme::controlRadius());
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairlineStrong,
                                ui_theme::accentBorder,
                                ui_theme::accentBorderStrong);
  button->setStyledBorderWidth(1);
  auto *text = makeText(std::move(label), 20, ui_theme::textPrimary);
  text->setAlign(TextView::CENTER);
  button->setContentView(text);
  return button;
}
} // namespace

void ChartRecordsScene::init() { buildView(); }

void ChartRecordsScene::buildView() {
  rootLayout_ =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout_->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12)
      ->setPadding(Edge::All, 24)
      ->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  addView(rootLayout_);

  auto *header = new View();
  header->setHeight(64)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(14);
  auto *back = makeButton("Back");
  back->setOnClickListener([this] { goBack(); });
  header->addView(back);
  auto *title = makeText("Records", 34, ui_theme::textPrimary);
  title->setWidth(160)->setHeight(52)->setFlexShrink(0);
  header->addView(title);
  auto *chartTitle = makeText(record_.meta.Title, 20, ui_theme::textSecondary);
  chartTitle->setFlex(1)->setHeight(52)->setMinWidth(0);
  header->addView(chartTitle);
  rootLayout_->addView(header);

  recordsView_ = new ResultRecordListView();
  recordsView_->setFlex(1)->setMinHeight(0);
  recordsView_->clearBackgroundColor();
  recordsView_->setThemedBorderColor(ui_theme::hairline);
  recordsView_->setBorderWidth(1);
  rootLayout_->addView(recordsView_);

  emptyText_ = makeText("No records.", 20, ui_theme::textSecondary);
  emptyText_->setHeight(48);
  emptyText_->setAlign(TextView::CENTER);
  emptyText_->setVisible(false);
  rootLayout_->addView(emptyText_);
  loadRecords();
  rootLayout_->applyYogaLayout();
  layoutWidth_ = rendering::window_width;
  layoutHeight_ = rendering::window_height;
}

void ChartRecordsScene::loadRecords() {
  if (recordsView_ == nullptr || emptyText_ == nullptr) return;
  std::vector<ResultRecordSummary> projected;
  const auto legacy = context.replayRepository.ListLegacyChartSummaries(
      record_.meta, kMaximumLegacyResultSummaryRows);
  projected.reserve(legacy.size());
  for (const LegacyChartResultSummary &summary : legacy) {
    projected.push_back(makeLegacyChartResultRecord(summary));
  }
  if (!record_.meta.SHA256.empty()) {
    const auto history = context.replayRepository.ListModernChartResults(
        record_.meta.SHA256, kMaximumModernChartHistoryRows);
    if (history.status == ModernChartHistoryReadStatus::Loaded) {
      replay::ReplayFileActionService replayActions(context.replayRepository);
      projected.reserve(projected.size() + history.records.size());
      for (const ModernChartResultRecord &modern : history.records) {
        const auto inspected = replayActions.probe(modern.replayFile);
        projected.push_back(makeModernChartResultRecord(
            modern, replay::replayStateForFileAction(inspected.state),
            ir::IrRecordState::Hidden));
      }
    }
  }
  const auto records = mergeResultRecords(
      std::span<const ReplaySummary>{}, projected,
      std::span<const ir::IrRemoteScore>{}, std::string_view{},
      std::string_view{});
  recordsView_->setResultRecords(records);
  emptyText_->setVisible(records.empty());
}

void ChartRecordsScene::goBack() {
  (void)returnToScene(*context.sceneManager, returnTarget_);
}

EventHandleResult ChartRecordsScene::handleEvents(SDL_Event &event) {
  if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
    goBack();
    return {};
  }
  return Scene::handleEvents(event);
}

void ChartRecordsScene::update(float) {
  if (rootLayout_ == nullptr ||
      (layoutWidth_ == rendering::window_width &&
       layoutHeight_ == rendering::window_height)) {
    return;
  }
  layoutWidth_ = rendering::window_width;
  layoutHeight_ = rendering::window_height;
  rootLayout_->setSize(layoutWidth_, layoutHeight_);
  rootLayout_->applyYogaLayout();
}

void ChartRecordsScene::renderScene() {}

void ChartRecordsScene::cleanupScene() {
  rootLayout_ = nullptr;
  recordsView_ = nullptr;
  emptyText_ = nullptr;
}
