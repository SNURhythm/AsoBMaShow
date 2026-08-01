#include "IrUploadCandidateListView.h"

#include "../ScoreRankUtils.h"
#include "../scene/play/GameplayGaugeTypes.h"
#include "ClearLampColors.h"
#include "UiTheme.h"

#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace {

constexpr const char *kUiFont = "assets/fonts/notosanscjkjp.ttf";

std::string formatGauge(float gauge) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << gauge << '%';
  return stream.str();
}

std::string keyModeDescription(int keyMode) {
  switch (keyMode) {
  case 5:
    return "5K";
  case 7:
    return "7K";
  case 10:
    return "5KDP";
  case 14:
    return "7KDP";
  default:
    return std::to_string(keyMode) + "K";
  }
}

std::string attemptDetail(
    const result_persistence::ModernChartResult &result) {
  std::string detail = "Attempt " + result.attemptId.substr(0, 8);
  detail += "  Combo " + std::to_string(result.score.maxCombo);
  detail += "  ";
  detail += gaugeTypeToShortLabel(result.adoptedGaugeType);
  detail += "  Gauge " + formatGauge(result.score.finalGauge);
  const auto &provenance = result.score.provenance;
  if (!provenance.player1.option.empty() &&
      provenance.player1.option != "NORMAL") {
    detail += "  " + provenance.player1.option;
  }
  if (assist_options::isEnabled(provenance.assistOption)) {
    detail += "  Assist " +
              assist_options::normalize(provenance.assistOption);
  }
  return detail;
}

} // namespace

IrUploadCandidateListItemView::IrUploadCandidateListItemView() {
  clearLamp_ = new View();
  selectionButton_ = new Button();
  selectionContent_ = new CheckboxButtonContent("", 20, 30);
  artworkFrame_ = new View();
  jacketImage_ = new ImageView(0, 0, 0, 0);
  textColumn_ = new View();
  titleText_ = new TextView(kUiFont, 22);
  artistText_ = new TextView(kUiFont, 15);
  attemptText_ = new TextView(kUiFont, 14);
  difficultyColumn_ = new View();
  difficultyText_ = new TextView(kUiFont, 18);
  keyModeText_ = new TextView(kUiFont, 14);
  scoreColumn_ = new View();
  scoreText_ = new TextView(kUiFont, 18);
  rankText_ = new TextView(kUiFont, 16);
  statusText_ = new TextView(kUiFont, 14);

  setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setPadding(Edge::All, 8)
      ->setGap(10)
      ->setThemedBackgroundColor(ui_theme::panelSubtle)
      ->setThemedBorderColor(ui_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::controlRadius());

  selectionButton_->setName("irUploadSelection");
  selectionButton_->setWidth(34)->setHeight(34)->setFlexShrink(0);
  selectionButton_->setCornerRadius(6.0F);
  selectionButton_->setContentView(selectionContent_);
  addView(selectionButton_);

  clearLamp_->setName("irUploadClearLamp");
  clearLamp_->setWidth(6)->setHeight(84)->setFlexShrink(0);
  clearLamp_->setCornerRadius(3.0F);
  addView(clearLamp_);

  artworkFrame_->setWidth(84)
      ->setHeight(84)
      ->setFlexShrink(0)
      ->setPadding(Edge::All, 2)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setThemedBackgroundColor(ui_theme::panelSubtle)
      ->setThemedBorderColor(ui_theme::hairlineSubtle)
      ->setBorderWidth(1)
      ->setCornerRadius(ui_theme::controlRadius());
  jacketImage_->setName("irUploadJacket");
  jacketImage_->setWidth(78)->setHeight(78);
  jacketImage_->setCornerRadius(
      ui_theme::childRadiusForInset(ui_theme::controlRadius(), 1.0F, 2.0F));
  artworkFrame_->addView(jacketImage_);
  addView(artworkFrame_);

  textColumn_->setFlexDirection(FlexDirection::Column)
      ->setJustifyContent(YGJustifyCenter)
      ->setFlexGrow(1)
      ->setFlexBasis(0)
      ->setMinWidth(0)
      ->setGap(2);
  titleText_->setName("irUploadTitle");
  titleText_->setHeight(27);
  titleText_->setOverflow(TextView::TextOverflow::Marquee);
  artistText_->setName("irUploadArtist");
  artistText_->setHeight(19);
  artistText_->setOverflow(TextView::TextOverflow::Marquee);
  attemptText_->setName("irUploadAttempt");
  attemptText_->setHeight(18);
  attemptText_->setOverflow(TextView::TextOverflow::Hidden);
  textColumn_->addView(titleText_);
  textColumn_->addView(artistText_);
  textColumn_->addView(attemptText_);
  addView(textColumn_);

  difficultyColumn_->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignFlexEnd)
      ->setJustifyContent(YGJustifyCenter)
      ->setWidth(70)
      ->setHeight(84)
      ->setFlexShrink(0)
      ->setGap(4);
  difficultyText_->setName("irUploadDifficulty");
  difficultyText_->setWidth(70)->setHeight(30);
  difficultyText_->setAlign(TextView::TextAlign::RIGHT);
  difficultyText_->setVAlign(TextView::TextVAlign::MIDDLE);
  keyModeText_->setName("irUploadKeyMode");
  keyModeText_->setWidth(70)->setHeight(22);
  keyModeText_->setAlign(TextView::TextAlign::RIGHT);
  keyModeText_->setVAlign(TextView::TextVAlign::MIDDLE);
  difficultyColumn_->addView(difficultyText_);
  difficultyColumn_->addView(keyModeText_);
  addView(difficultyColumn_);

  scoreColumn_->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignFlexEnd)
      ->setJustifyContent(YGJustifyCenter)
      ->setWidth(78)
      ->setHeight(84)
      ->setFlexShrink(0)
      ->setGap(2);
  scoreText_->setName("irUploadScore");
  scoreText_->setWidth(78)->setHeight(28);
  scoreText_->setAlign(TextView::TextAlign::RIGHT);
  scoreText_->setVAlign(TextView::TextVAlign::MIDDLE);
  rankText_->setName("irUploadRank");
  rankText_->setWidth(78)->setHeight(22);
  rankText_->setAlign(TextView::TextAlign::RIGHT);
  rankText_->setVAlign(TextView::TextVAlign::MIDDLE);
  scoreColumn_->addView(scoreText_);
  scoreColumn_->addView(rankText_);
  addView(scoreColumn_);

  statusText_->setName("irUploadStatus");
  statusText_->setWidth(58)->setHeight(28)->setFlexShrink(0);
  statusText_->setAlign(TextView::TextAlign::CENTER);
  statusText_->setVAlign(TextView::TextVAlign::MIDDLE);
  statusText_->setCornerRadius(6.0F);
  addView(statusText_);
}

void IrUploadCandidateListItemView::setCandidate(
    const ir::IrUploadCandidate &candidate, bool selected, bool selectionLocked,
    std::function<void(std::string)> selectionToggle) {
  const auto &result = candidate.result;
  const auto &score = result.score;
  const std::string attemptId = result.attemptId;

  selectionButton_->setOnClickListener({});
  selectionButton_->setSelected(selected);
  selectionButton_->setEnabled(true);
  selectionContent_->setChecked(selected);
  if (selected) {
    selectionContent_->setThemedColor(ui_theme::cyan);
  } else {
    selectionContent_->setThemedColor(ui_theme::textMuted);
  }
  if (!selectionLocked) {
    selectionButton_->setOnClickListener(
        [attemptId, selectionToggle = std::move(selectionToggle)]() {
          if (selectionToggle) {
            selectionToggle(attemptId);
          }
        });
  }

  titleText_->setText(score.chartTitle);
  artistText_->setText(score.chartArtist);
  std::string attempt = attemptDetail(result);
  if (!candidate.failureReason.empty()) {
    attempt += "  Failed: " + candidate.failureReason;
  }
  attemptText_->setText(attempt);
  difficultyText_->setText("");
  keyModeText_->setText(keyModeDescription(result.keyMode));

  if (score.maxScore > 0) {
    scoreText_->setText(std::to_string(score.score));
    rankText_->setText(
        score_rank::displayLabelForScore(score.score, score.maxScore));
  } else {
    scoreText_->setText("");
    rankText_->setText("");
  }
  scoreText_->setThemedColor(ui_theme::cyan);
  rankText_->setThemedColor(ui_theme::amber);

  if (hasClearLampColor(score.clearType)) {
    clearLamp_->setBackgroundColor(clearLampColorForRank(score.clearType));
    hasClearLamp_ = true;
  } else {
    clearLamp_->clearBackgroundColor();
    hasClearLamp_ = false;
  }

  jacketImage_->freeImage();

  const bool failed = candidate.state == ir::IrRecordState::Failed;
  statusText_->setText(failed ? "Retry" : "Eligible");
  if (failed) {
    statusText_->setThemedBackgroundColor(ui_theme::coral);
    statusText_->setThemedColor(
        [] { return ui_theme::textOn(ui_theme::coral()); });
  } else {
    statusText_->setThemedBackgroundColor(ui_theme::amber);
    statusText_->setThemedColor(
        [] { return ui_theme::textOn(ui_theme::amber()); });
  }
  titleText_->setThemedColor(ui_theme::textPrimary);
  artistText_->setThemedColor(ui_theme::textSecondary);
  attemptText_->setThemedColor(ui_theme::textMuted);
}

IrUploadCandidateListView::IrUploadCandidateListView()
    : RecyclerView<ir::IrUploadCandidate>(
          [](const ir::IrUploadCandidate &left,
             const ir::IrUploadCandidate &right) {
            return left.attemptId() == right.attemptId();
          }) {
  itemHeight = 108;
  onCreateView = [](const ir::IrUploadCandidate &) {
    return new IrUploadCandidateListItemView();
  };
  onBind = [this](View *view, const ir::IrUploadCandidate &candidate, int,
                  bool) {
    auto *itemView = dynamic_cast<IrUploadCandidateListItemView *>(view);
    if (itemView == nullptr) {
      return;
    }
    const bool selected =
        selectedAttemptIds_.contains(candidate.result.attemptId);
    itemView->setCandidate(candidate, selected, selectionLocked_,
                           [this](std::string attemptId) {
                             if (onSelectionToggle) {
                               onSelectionToggle(std::move(attemptId));
                             }
                           });
  };
}

void IrUploadCandidateListView::setCandidates(
    const std::vector<ir::IrUploadCandidate> &candidates,
    const std::unordered_set<std::string> &selectedAttemptIds) {
  selectedAttemptIds_ = selectedAttemptIds;
  setItems(candidates);
}

void IrUploadCandidateListView::setSelectedAttemptIds(
    const std::unordered_set<std::string> &selectedAttemptIds) {
  selectedAttemptIds_ = selectedAttemptIds;
  rebindVisibleItems();
}

void IrUploadCandidateListView::setSelectionLocked(bool locked) {
  if (selectionLocked_ == locked) {
    return;
  }
  selectionLocked_ = locked;
  rebindVisibleItems();
}
