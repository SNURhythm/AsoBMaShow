#include "IrRankingModal.h"

#include "IrRankingService.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/ClearLampColors.h"
#include "../view/IconText.h"
#include "../view/OverlayPortal.h"
#include "../view/RecyclerView.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace ir {
namespace {

constexpr const char *kFont = "assets/fonts/notosanscjkjp.ttf";
constexpr uint32_t kIconXmark = 0xf00d;
constexpr int kPanelMaximumWidth = 1180;
constexpr int kPanelMaximumHeight = 840;
constexpr int kPanelMargin = 24;
constexpr float kRankingRowHorizontalPadding = 12.0F;
constexpr float kRankingColumnGap = 10.0F;
constexpr float kRankColumnWidth = 58.0F;
constexpr float kScoreColumnWidth = 152.0F;
constexpr float kRateColumnWidth = 86.0F;
constexpr float kLampColumnWidth = 174.0F;
constexpr float kCompactLampColumnWidth = 144.0F;
constexpr float kBadPointsColumnWidth = 62.0F;
constexpr float kMaxComboColumnWidth = 88.0F;
constexpr float kAchievedColumnWidth = 172.0F;

struct SafeInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

SafeInsets safeInsets() {
  SafeInsets result;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const auto normalized = GetIOSSafeAreaInsetsNormalized();
  result.top = static_cast<int>(std::lround(
      normalized.top * static_cast<float>(rendering::window_height)));
  result.left = static_cast<int>(std::lround(
      normalized.left * static_cast<float>(rendering::window_width)));
  result.bottom = static_cast<int>(std::lround(
      normalized.bottom * static_cast<float>(rendering::window_height)));
  result.right = static_cast<int>(std::lround(
      normalized.right * static_cast<float>(rendering::window_width)));
#endif
  return result;
}

TextView *makeText(int size, TextView::TextAlign align = TextView::LEFT) {
  auto *text = new TextView(kFont, size);
  text->setAlign(align);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  text->setThemedColor(ui_theme::textPrimary);
  return text;
}

Button *makeActionButton(const std::string &label, int width,
                         std::function<void()> action) {
  auto *button = new Button();
  auto *text = makeText(19, TextView::CENTER);
  text->setText(label);
  button->setContentView(text);
  button->setWidth(width)->setHeight(48)->setFlexShrink(0);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairlineStrong,
                                ui_theme::accentBorderStrong,
                                ui_theme::accentBorderStrong);
  button->setStyledBorderWidth(1);
  button->setOnClickListener(std::move(action));
  return button;
}

Button *makeIconActionButton(uint32_t codepoint,
                             std::function<void()> action) {
  auto *button = new Button();
  auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 22);
  icon->setText(ui_icons::textForCodepoint(codepoint));
  icon->setAlign(TextView::CENTER);
  icon->setVAlign(TextView::MIDDLE);
  icon->setThemedColor(ui_theme::textPrimary);
  button->setContentView(icon);
  button->setWidth(48)->setHeight(48)->setFlexShrink(0);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairlineStrong,
                                ui_theme::accentBorderStrong,
                                ui_theme::accentBorderStrong);
  button->setStyledBorderWidth(1);
  button->setOnClickListener(std::move(action));
  return button;
}

View *makeMetricCard(std::string label, TextView *&value, int valueSize = 22) {
  auto *card = new View();
  card->setFlex(1.0F)->setMinWidth(0);
  card->setFlexDirection(FlexDirection::Column);
  card->setPadding(Edge::All, 10);
  card->setGap(4);
  card->setThemedBackgroundColor(ui_theme::panelSubtle);
  card->setCornerRadius(ui_theme::controlRadius());
  auto *caption = makeText(14);
  caption->setText(std::move(label));
  caption->setThemedColor(ui_theme::textMuted);
  value = makeText(valueSize);
  card->addView(caption);
  card->addView(value);
  return card;
}

class ModalScrim final : public View {
public:
  explicit ModalScrim(std::function<void()> requestClose)
      : requestClose_(std::move(requestClose)) {}

private:
  bool handleEventsImpl(SDL_Event &event) override {
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
        (event.key.keysym.sym == SDLK_ESCAPE ||
         event.key.keysym.sym == SDLK_AC_BACK)) {
      requestClose_();
    }
    return false;
  }

  std::function<void()> requestClose_;
};

class RankingRowView final : public View {
public:
  RankingRowView() {
    setFlexDirection(FlexDirection::Column);
    setAlignItems(YGAlignStretch);
    setPadding(Edge::Left, kRankingRowHorizontalPadding);
    setPadding(Edge::Right, kRankingRowHorizontalPadding);
    setPadding(Edge::Top, 5);
    setPadding(Edge::Bottom, 5);
    setGap(2);
    setCornerRadius(ui_theme::controlRadius());
    setBorderWidth(1);

    primary_ = new View();
    primary_->setFlexDirection(FlexDirection::Row);
    primary_->setAlignItems(YGAlignCenter);
    primary_->setGap(kRankingColumnGap);
    primary_->setFlex(1);

    rank_ = makeText(18, TextView::CENTER);
    rank_->setWidth(kRankColumnWidth);
    player_ = makeText(19);
    player_->setFlex(1);
    score_ = makeText(17, TextView::RIGHT);
    score_->setWidth(kScoreColumnWidth);
    rate_ = makeText(17, TextView::RIGHT);
    rate_->setWidth(kRateColumnWidth);
    lamp_ = makeText(15, TextView::CENTER);
    lamp_->setWidth(kLampColumnWidth)->setHeight(32);
    lamp_->setCornerRadius(6);
    badPoints_ = makeText(17, TextView::RIGHT);
    badPoints_->setWidth(kBadPointsColumnWidth);
    combo_ = makeText(17, TextView::RIGHT);
    combo_->setWidth(kMaxComboColumnWidth);
    time_ = makeText(15, TextView::RIGHT);
    time_->setWidth(kAchievedColumnWidth);
    primary_->addView(rank_);
    primary_->addView(player_);
    primary_->addView(score_);
    primary_->addView(rate_);
    primary_->addView(lamp_);
    primary_->addView(badPoints_);
    primary_->addView(combo_);
    primary_->addView(time_);
    addView(primary_);
  }

  void bind(const IrRankingRowPresentation &row) {
    rank_->setText(row.rankText);
    player_->setText(row.playerText);
    score_->setText(row.scoreText);
    rate_->setText(row.rateText);
    lamp_->setText(row.lampText);
    badPoints_->setText(row.badPointsText);
    combo_->setText(row.maxComboText);
    time_->setText(row.achievementTimeText);
    const Color lampColor = clearLampColorForRank(row.clearType);
    lamp_->setBackgroundColor(lampColor);
    lamp_->setColor(ui_theme::sdl(ui_theme::textOn(lampColor)));
    setBackgroundColor(row.highlighted
                           ? ui_theme::withAlpha(ui_theme::cyan(), 42)
                           : ui_theme::panelSubtle());
    setBorderColor(row.highlighted ? ui_theme::accentBorderStrong()
                                   : ui_theme::hairlineSubtle());
    player_->setThemedColor(row.highlighted ? ui_theme::cyan
                                            : ui_theme::textPrimary);

    score_->setDisplay(row.compact ? YGDisplayNone : YGDisplayFlex);
    badPoints_->setDisplay(row.showBadPoints && !row.compact ? YGDisplayFlex
                                                             : YGDisplayNone);
    combo_->setDisplay(row.showMaxCombo && !row.compact ? YGDisplayFlex
                                                        : YGDisplayNone);
    time_->setDisplay(row.showAchievementTime && !row.compact ? YGDisplayFlex
                                                              : YGDisplayNone);
    lamp_->setWidth(row.compact ? kCompactLampColumnWidth
                                : kLampColumnWidth);
    applyYogaLayout();
  }

private:
  View *primary_ = nullptr;
  TextView *rank_ = nullptr;
  TextView *player_ = nullptr;
  TextView *score_ = nullptr;
  TextView *rate_ = nullptr;
  TextView *lamp_ = nullptr;
  TextView *badPoints_ = nullptr;
  TextView *combo_ = nullptr;
  TextView *time_ = nullptr;
};

TextView *makeHeaderLabel(std::string label, TextView::TextAlign align) {
  auto *text = makeText(14, align);
  text->setText(std::move(label));
  text->setThemedColor(ui_theme::textMuted);
  return text;
}

class RankingTableHeaderView final : public View {
public:
  RankingTableHeaderView() {
    setFlexDirection(FlexDirection::Row);
    setAlignItems(YGAlignCenter);
    setPadding(Edge::Left, kRankingRowHorizontalPadding);
    setPadding(Edge::Right, kRankingRowHorizontalPadding);
    setGap(kRankingColumnGap);
    setHeight(34);
    setFlexShrink(0);
    setThemedBackgroundColor(ui_theme::fieldInk);
    setCornerRadius(ui_theme::controlRadius());

    rank_ = makeHeaderLabel("Rank", TextView::CENTER);
    rank_->setWidth(kRankColumnWidth);
    player_ = makeHeaderLabel("Player", TextView::LEFT);
    player_->setFlex(1);
    score_ = makeHeaderLabel("EX Score", TextView::RIGHT);
    score_->setWidth(kScoreColumnWidth);
    rate_ = makeHeaderLabel("EX Rate", TextView::RIGHT);
    rate_->setWidth(kRateColumnWidth);
    lamp_ = makeHeaderLabel("Lamp", TextView::CENTER);
    lamp_->setWidth(kLampColumnWidth);
    badPoints_ = makeHeaderLabel("BP", TextView::RIGHT);
    badPoints_->setWidth(kBadPointsColumnWidth);
    combo_ = makeHeaderLabel("Max Combo", TextView::RIGHT);
    combo_->setWidth(kMaxComboColumnWidth);
    time_ = makeHeaderLabel("Achieved", TextView::RIGHT);
    time_->setWidth(kAchievedColumnWidth);

    addView(rank_);
    addView(player_);
    addView(score_);
    addView(rate_);
    addView(lamp_);
    addView(badPoints_);
    addView(combo_);
    addView(time_);
  }

  void bind(int width) {
    const bool compact = useCompactIrRankingColumns(width);
    score_->setDisplay(compact ? YGDisplayNone : YGDisplayFlex);
    badPoints_->setDisplay(compact ? YGDisplayNone : YGDisplayFlex);
    combo_->setDisplay(compact ? YGDisplayNone : YGDisplayFlex);
    time_->setDisplay(compact ? YGDisplayNone : YGDisplayFlex);
    lamp_->setWidth(compact ? kCompactLampColumnWidth : kLampColumnWidth);
    applyYogaLayout();
  }

private:
  TextView *rank_ = nullptr;
  TextView *player_ = nullptr;
  TextView *score_ = nullptr;
  TextView *rate_ = nullptr;
  TextView *lamp_ = nullptr;
  TextView *badPoints_ = nullptr;
  TextView *combo_ = nullptr;
  TextView *time_ = nullptr;
};

std::string comparisonText(const IrLocalComparison &comparison) {
  std::string result =
      comparison.label + "   EX " + std::to_string(comparison.score) + " / " +
      std::to_string(comparison.maxScore) + "   " +
      formatIrRankingRate(comparison.score, comparison.maxScore) + "   " +
      clearTypeRankToLabel(comparison.clearType);
  result +=
      "   BP " + (comparison.badPoints ? std::to_string(*comparison.badPoints)
                                       : "\xE2\x80\x94");
  result +=
      "   Combo " + (comparison.maxCombo ? std::to_string(*comparison.maxCombo)
                                         : "\xE2\x80\x94");
  return result;
}

} // namespace

struct IrRankingModal::Impl {
  OverlayPortal &portal;
  IrRankingService &service;
  IrRankingModalModel model;
  ModalScrim *root = nullptr;
  View *panel = nullptr;
  TextView *headerTitle = nullptr;
  TextView *chartTitle = nullptr;
  TextView *fetchedAt = nullptr;
  Button *refreshButton = nullptr;
  Button *closeButton = nullptr;
  View *comparisonCard = nullptr;
  TextView *comparison = nullptr;
  TextView *status = nullptr;
  TextView *detail = nullptr;
  Button *retryButton = nullptr;
  View *rankingTable = nullptr;
  RankingTableHeaderView *tableHeader = nullptr;
  RecyclerView<IrChartRankingEntry> *list = nullptr;
  TextView *paginationStatus = nullptr;
  ModalScrim *scoreDetailRoot = nullptr;
  View *scoreDetailPanel = nullptr;
  TextView *scoreDetailTitle = nullptr;
  TextView *scoreDetailScore = nullptr;
  TextView *scoreDetailRate = nullptr;
  TextView *scoreDetailLamp = nullptr;
  View *scoreDetailJudgements = nullptr;
  TextView *scoreDetailJudgementUnavailable = nullptr;
  TextView *scoreDetailKpoorNote = nullptr;
  std::array<TextView *, 6> scoreDetailJudgementLabels{};
  std::array<TextView *, 3> scoreDetailJudgementHeadings{};
  std::array<TextView *, 5> scoreDetailTotals{};
  std::array<TextView *, 5> scoreDetailEarly{};
  std::array<TextView *, 5> scoreDetailLate{};
  TextView *scoreDetailBadPoints = nullptr;
  TextView *scoreDetailMaxCombo = nullptr;
  TextView *scoreDetailAchievementTime = nullptr;
  std::shared_ptr<const IrChartRanking> visibleRanking;
  bool open = false;
  bool closeRequested = false;
  bool scoreDetailOpen = false;
  int layoutWidth = 0;
  int layoutHeight = 0;
  SafeInsets layoutSafe;

  Impl(OverlayPortal &portalValue, IrRankingService &serviceValue)
      : portal(portalValue), service(serviceValue) {
    build();
  }

  ~Impl() {
    closeNow();
    delete root;
    delete scoreDetailRoot;
  }

  void requestClose() {
    closeRequested = true;
    if (root != nullptr) {
      root->setVisible(false);
    }
  }

  void build() {
    panel = new View();
    root = new ModalScrim([this]() { requestClose(); });
    root->setPositionType(YGPositionTypeAbsolute);
    root->setPosition(Edge::Left, 0);
    root->setPosition(Edge::Top, 0);
    root->setFlexDirection(FlexDirection::Column);
    root->setAlignItems(YGAlignCenter);
    root->setJustifyContent(YGJustifyCenter);
    root->setBackgroundColor(Color(2, 5, 9, 190));

    panel->setFlexDirection(FlexDirection::Column);
    panel->setAlignItems(YGAlignStretch);
    panel->setPadding(Edge::All, 22);
    panel->setGap(12);
    panel->setThemedBackgroundColor(ui_theme::panelStrong);
    panel->setCornerRadius(ui_theme::panelRadius());
    panel->setThemedBorderColor(ui_theme::hairlineStrong);
    panel->setBorderWidth(1);
    panel->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow);

    auto *header = new View();
    header->setFlexDirection(FlexDirection::Row);
    header->setAlignItems(YGAlignCenter);
    header->setGap(10);
    header->setHeight(52);
    header->setFlexShrink(0);
    headerTitle = makeText(28);
    headerTitle->setText("Bokutachi Ranking");
    headerTitle->setFlex(1);
    fetchedAt = makeText(15, TextView::RIGHT);
    fetchedAt->setThemedColor(ui_theme::textMuted);
    fetchedAt->setWidth(220);
    refreshButton = makeActionButton("Refresh", 112, [this]() { refresh(); });
    closeButton =
        makeIconActionButton(kIconXmark, [this]() { requestClose(); });
    header->addView(headerTitle);
    header->addView(fetchedAt);
    header->addView(refreshButton);
    header->addView(closeButton);
    panel->addView(header);

    chartTitle = makeText(21);
    chartTitle->setThemedColor(ui_theme::textSecondary);
    chartTitle->setHeight(32);
    chartTitle->setFlexShrink(0);
    panel->addView(chartTitle);

    comparisonCard = new View();
    comparisonCard->setHeight(54);
    comparisonCard->setFlexShrink(0);
    comparisonCard->setPadding(Edge::Left, 14);
    comparisonCard->setPadding(Edge::Right, 14);
    comparisonCard->setThemedBackgroundColor(ui_theme::panelSubtle);
    comparisonCard->setCornerRadius(ui_theme::controlRadius());
    comparisonCard->setThemedBorderColor(ui_theme::accentBorder);
    comparisonCard->setBorderWidth(1);
    comparison = makeText(17);
    comparisonCard->addView(comparison);
    panel->addView(comparisonCard);

    status = makeText(21, TextView::CENTER);
    status->setWrap(true);
    status->setHeight(64);
    status->setFlexShrink(0);
    panel->addView(status);
    detail = makeText(16, TextView::CENTER);
    detail->setWrap(true);
    detail->setThemedColor(ui_theme::textSecondary);
    detail->setHeight(54);
    detail->setFlexShrink(0);
    panel->addView(detail);
    retryButton = makeActionButton("Retry", 140, [this]() { refresh(); });
    retryButton->setAlignSelf(YGAlignCenter);
    panel->addView(retryButton);

    rankingTable = new View();
    rankingTable->setFlexDirection(FlexDirection::Column);
    rankingTable->setAlignItems(YGAlignStretch);
    rankingTable->setFlexGrow(1);
    rankingTable->setFlexShrink(1);
    rankingTable->setFlexBasis(0);
    rankingTable->setMinHeight(0);
    rankingTable->setGap(4);
    tableHeader = new RankingTableHeaderView();
    rankingTable->addView(tableHeader);

    list = new RecyclerView<IrChartRankingEntry>(
        [](const auto &left, const auto &right) {
          return left.rank == right.rank && left.playerName == right.playerName;
        });
    list->setFlexGrow(1);
    list->setFlexShrink(1);
    list->setFlexBasis(0);
    list->setMinHeight(0);
    list->itemHeight = 74;
    list->reserveScrollbarGutter = true;
    list->clearBackgroundColor();
    list->setCornerRadius(ui_theme::controlRadius());
    list->setThemedBorderColor(ui_theme::hairlineSubtle);
    list->setBorderWidth(1);
    list->onCreateView = [](const auto &) { return new RankingRowView(); };
    list->onBind = [this](View *view, const auto &, int index, bool) {
      auto *row = dynamic_cast<RankingRowView *>(view);
      if (row != nullptr) {
        row->bind(model.row(index, view->getWidth()));
      }
    };
    list->onSelected = [this](const auto &, int index) {
      showScoreDetails(index);
    };
    rankingTable->addView(list);
    panel->addView(rankingTable);
    paginationStatus = makeText(15, TextView::CENTER);
    paginationStatus->setThemedColor(ui_theme::textMuted);
    paginationStatus->setWrap(true);
    paginationStatus->setHeight(44);
    paginationStatus->setFlexShrink(0);
    paginationStatus->setDisplay(YGDisplayNone);
    panel->addView(paginationStatus);
    root->addView(panel);
    root->setVisible(false);
    buildScoreDetail();
  }

  void buildScoreDetail() {
    scoreDetailPanel = new View();
    scoreDetailRoot =
        new ModalScrim([this]() { hideScoreDetails(); });
    scoreDetailRoot->setPositionType(YGPositionTypeAbsolute);
    scoreDetailRoot->setPosition(Edge::Left, 0);
    scoreDetailRoot->setPosition(Edge::Top, 0);
    scoreDetailRoot->setFlexDirection(FlexDirection::Column);
    scoreDetailRoot->setAlignItems(YGAlignCenter);
    scoreDetailRoot->setJustifyContent(YGJustifyCenter);
    scoreDetailRoot->setBackgroundColor(Color(2, 5, 9, 214));

    scoreDetailPanel->setFlexDirection(FlexDirection::Column);
    scoreDetailPanel->setAlignItems(YGAlignStretch);
    scoreDetailPanel->setPadding(Edge::All, 18);
    scoreDetailPanel->setGap(12);
    scoreDetailPanel->setThemedBackgroundColor(ui_theme::panelStrong);
    scoreDetailPanel->setCornerRadius(ui_theme::panelRadius());
    scoreDetailPanel->setThemedBorderColor(ui_theme::hairlineStrong);
    scoreDetailPanel->setBorderWidth(1);
    scoreDetailPanel->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow);

    auto *detailHeader = new View();
    detailHeader->setFlexDirection(FlexDirection::Row);
    detailHeader->setAlignItems(YGAlignCenter);
    detailHeader->setGap(10)->setHeight(52)->setFlexShrink(0);
    scoreDetailTitle = makeText(25);
    scoreDetailTitle->setFlex(1);
    detailHeader->addView(scoreDetailTitle);
    detailHeader->addView(
        makeIconActionButton(kIconXmark, [this]() { hideScoreDetails(); }));

    const auto makeMetricRow = [] {
      auto *row = new View();
      row->setFlexDirection(FlexDirection::Row);
      row->setAlignItems(YGAlignStretch);
      row->setGap(10)->setFlexShrink(0);
      return row;
    };
    auto *summary = makeMetricRow();
    summary->setHeight(82);
    summary->addView(makeMetricCard("EX Score", scoreDetailScore));
    summary->addView(makeMetricCard("Rate", scoreDetailRate));
    summary->addView(makeMetricCard("Lamp", scoreDetailLamp, 17));

    scoreDetailJudgements = new View();
    scoreDetailJudgements->setFlexDirection(FlexDirection::Column);
    scoreDetailJudgements->setAlignItems(YGAlignStretch);
    scoreDetailJudgements->setHeight(230);
    scoreDetailJudgements->setFlexShrink(0);
    scoreDetailJudgements->setGap(2);
    scoreDetailJudgements->setPadding(Edge::All, 4);
    scoreDetailJudgements->setThemedBackgroundColor(ui_theme::fieldInk);
    scoreDetailJudgements->setCornerRadius(ui_theme::controlRadius());

    const auto makeJudgementHeaderCell = [] {
      auto *cell = makeText(14, TextView::RIGHT);
      cell->setThemedColor(ui_theme::textMuted);
      return cell;
    };
    auto *judgementHeader = new View();
    judgementHeader->setFlexDirection(FlexDirection::Row);
    judgementHeader->setAlignItems(YGAlignCenter);
    judgementHeader->setHeight(30)->setFlexShrink(0);
    judgementHeader->setPadding(Edge::Left, 10);
    judgementHeader->setPadding(Edge::Right, 10);
    auto *judgementHeading = makeText(14);
    judgementHeading->setText("Judgment");
    judgementHeading->setThemedColor(ui_theme::textMuted);
    judgementHeading->setWidth(152);
    scoreDetailJudgementLabels[0] = judgementHeading;
    auto *totalHeading = makeJudgementHeaderCell();
    totalHeading->setText("Total");
    scoreDetailJudgementHeadings[0] = totalHeading;
    auto *earlyHeading = makeJudgementHeaderCell();
    earlyHeading->setText("Early");
    earlyHeading->setThemedColor(ui_theme::fastFeedback);
    scoreDetailJudgementHeadings[1] = earlyHeading;
    auto *lateHeading = makeJudgementHeaderCell();
    lateHeading->setText("Late");
    lateHeading->setThemedColor(ui_theme::slowFeedback);
    scoreDetailJudgementHeadings[2] = lateHeading;
    judgementHeader->addView(judgementHeading);
    judgementHeader->addView(totalHeading);
    judgementHeader->addView(earlyHeading);
    judgementHeader->addView(lateHeading);
    scoreDetailJudgements->addView(judgementHeader);

    const auto makeJudgementRow =
        [this](std::string label, std::size_t index,
               View::ThemeColorProvider labelColor) {
          auto *row = new View();
          row->setFlexDirection(FlexDirection::Row);
          row->setAlignItems(YGAlignCenter);
          row->setHeight(36)->setFlexShrink(0);
          row->setPadding(Edge::Left, 10);
          row->setPadding(Edge::Right, 10);
          row->setThemedBackgroundColor(ui_theme::panelSubtle);

          auto *name = makeText(16);
          name->setText(std::move(label));
          name->setThemedColor(std::move(labelColor));
          name->setWidth(152);
          scoreDetailJudgementLabels[index + 1] = name;
          scoreDetailTotals[index] = makeText(17, TextView::RIGHT);
          scoreDetailEarly[index] = makeText(17, TextView::RIGHT);
          scoreDetailEarly[index]->setThemedColor(ui_theme::fastFeedback);
          scoreDetailLate[index] = makeText(17, TextView::RIGHT);
          scoreDetailLate[index]->setThemedColor(ui_theme::slowFeedback);
          row->addView(name);
          row->addView(scoreDetailTotals[index]);
          row->addView(scoreDetailEarly[index]);
          row->addView(scoreDetailLate[index]);
          scoreDetailJudgements->addView(row);
        };
    makeJudgementRow("PGREAT", 0, ui_theme::cyan);
    makeJudgementRow("GREAT", 1, ui_theme::lime);
    makeJudgementRow("GOOD", 2, ui_theme::amber);
    makeJudgementRow("BAD", 3, [] { return Color(255, 132, 96, 255); });
    makeJudgementRow("POOR", 4, ui_theme::coral);

    scoreDetailJudgementUnavailable = makeText(17, TextView::CENTER);
    scoreDetailJudgementUnavailable->setText(
        "Judgement breakdown unavailable from Bokutachi rankings.");
    scoreDetailJudgementUnavailable->setWrap(true);
    scoreDetailJudgementUnavailable->setHeight(88);
    scoreDetailJudgementUnavailable->setFlexShrink(0);
    scoreDetailJudgementUnavailable->setPadding(Edge::All, 12);
    scoreDetailJudgementUnavailable->setThemedColor(ui_theme::textSecondary);
    scoreDetailJudgementUnavailable->setThemedBackgroundColor(
        ui_theme::panelSubtle);
    scoreDetailJudgementUnavailable->setCornerRadius(ui_theme::controlRadius());

    scoreDetailKpoorNote = makeText(14, TextView::CENTER);
    scoreDetailKpoorNote->setText(
        "KPOOR is not exposed separately by Bokutachi; BP remains aggregate.");
    scoreDetailKpoorNote->setWrap(true);
    scoreDetailKpoorNote->setHeight(32);
    scoreDetailKpoorNote->setFlexShrink(0);
    scoreDetailKpoorNote->setThemedColor(ui_theme::textMuted);

    auto *metadata = makeMetricRow();
    metadata->setHeight(82);
    metadata->addView(makeMetricCard("BP", scoreDetailBadPoints));
    metadata->addView(makeMetricCard("Max Combo", scoreDetailMaxCombo));
    metadata->addView(
        makeMetricCard("Achieved", scoreDetailAchievementTime, 16));

    scoreDetailPanel->addView(detailHeader);
    scoreDetailPanel->addView(summary);
    scoreDetailPanel->addView(scoreDetailJudgements);
    scoreDetailPanel->addView(scoreDetailJudgementUnavailable);
    scoreDetailPanel->addView(scoreDetailKpoorNote);
    scoreDetailPanel->addView(metadata);
    scoreDetailRoot->addView(scoreDetailPanel);
    scoreDetailRoot->setVisible(false);
  }

  void applyScoreDetailJudgementColumnLayout() {
    constexpr float kTableHorizontalPadding = 8.0f;
    constexpr float kRowHorizontalPadding = 20.0f;
    const auto columns = layoutIrRankingJudgementColumns(
        static_cast<float>(scoreDetailJudgements->getWidth()) -
        kTableHorizontalPadding - kRowHorizontalPadding);
    const auto fixWidth = [](View *cell, float width) {
      cell->setWidth(width);
      cell->setFlexGrow(0);
      cell->setFlexBasis(width);
      cell->setFlexShrink(0);
    };
    for (auto *label : scoreDetailJudgementLabels) {
      fixWidth(label, columns.labelWidth);
    }
    for (auto *heading : scoreDetailJudgementHeadings) {
      fixWidth(heading, columns.valueWidth);
    }
    for (std::size_t row = 0; row < scoreDetailTotals.size(); ++row) {
      fixWidth(scoreDetailTotals[row], columns.valueWidth);
      fixWidth(scoreDetailEarly[row], columns.valueWidth);
      fixWidth(scoreDetailLate[row], columns.valueWidth);
    }
  }

  void updateScoreDetailLayout(const SafeInsets &safe) {
    const auto geometry =
        layoutIrRankingPanel({.viewportWidth = rendering::window_width,
                              .viewportHeight = rendering::window_height,
                              .safeTop = safe.top,
                              .safeLeft = safe.left,
                              .safeBottom = safe.bottom,
                              .safeRight = safe.right,
                              .margin = 36,
                              .maximumWidth = 760,
                              .maximumHeight = 620});
    scoreDetailRoot->setSize(rendering::window_width, rendering::window_height);
    scoreDetailRoot->setPadding(Edge::Top, safe.top + 36);
    scoreDetailRoot->setPadding(Edge::Bottom, safe.bottom + 36);
    scoreDetailRoot->setPadding(Edge::Left, safe.left + 36);
    scoreDetailRoot->setPadding(Edge::Right, safe.right + 36);
    scoreDetailPanel->setWidth(static_cast<float>(geometry.width));
    scoreDetailPanel->setHeight(static_cast<float>(geometry.height));
    scoreDetailRoot->applyYogaLayout();
    applyScoreDetailJudgementColumnLayout();
    scoreDetailRoot->applyYogaLayout();
  }

  void hideScoreDetails() {
    portal.dismiss(scoreDetailRoot);
    scoreDetailRoot->setVisible(false);
    scoreDetailOpen = false;
  }

  void showScoreDetails(int index) {
    const auto detailValue = model.scoreDetail(index);
    if (!detailValue) {
      return;
    }
    hideScoreDetails();
    const auto &detail = *detailValue;
    scoreDetailTitle->setText(detail.rankText + "   " + detail.playerText);
    scoreDetailScore->setText(detail.scoreText);
    scoreDetailRate->setText(detail.rateText);
    scoreDetailLamp->setText(detail.lampText);
    const std::array totals{detail.totalPGreatText, detail.totalGreatText,
                            detail.totalGoodText, detail.totalBadText,
                            detail.totalPoorText};
    const std::array early{detail.earlyPGreatText, detail.earlyGreatText,
                           detail.earlyGoodText, detail.earlyBadText,
                           detail.earlyPoorText};
    const std::array late{detail.latePGreatText, detail.lateGreatText,
                          detail.lateGoodText, detail.lateBadText,
                          detail.latePoorText};
    for (std::size_t row = 0; row < totals.size(); ++row) {
      scoreDetailTotals[row]->setText(totals[row]);
      scoreDetailEarly[row]->setText(early[row]);
      scoreDetailLate[row]->setText(late[row]);
    }
    scoreDetailJudgements->setVisible(detail.judgementBreakdownAvailable);
    scoreDetailJudgements->setDisplay(
        detail.judgementBreakdownAvailable ? YGDisplayFlex : YGDisplayNone);
    scoreDetailJudgementUnavailable->setVisible(
        !detail.judgementBreakdownAvailable);
    scoreDetailJudgementUnavailable->setDisplay(
        detail.judgementBreakdownAvailable ? YGDisplayNone : YGDisplayFlex);
    scoreDetailBadPoints->setText(detail.badPointsText);
    scoreDetailMaxCombo->setText(detail.maxComboText);
    scoreDetailAchievementTime->setText(detail.achievementTimeText);
    const Color lampColor = clearLampColorForRank(detail.clearType);
    scoreDetailLamp->setBackgroundColor(lampColor);
    scoreDetailLamp->setColor(ui_theme::sdl(ui_theme::textOn(lampColor)));
    scoreDetailLamp->setCornerRadius(6);
    scoreDetailTitle->setThemedColor(
        detail.highlighted ? ui_theme::cyan : ui_theme::textPrimary);
    scoreDetailOpen = true;
    scoreDetailRoot->setVisible(true);
    updateScoreDetailLayout(safeInsets());
    portal.present(scoreDetailRoot);
  }

  void syncRankingHeader() {
    const int width = list->getVisibleItemWidth();
    if (width <= 0) {
      return;
    }
    tableHeader->setWidth(static_cast<float>(width));
    tableHeader->bind(width);
  }

  void updateLayout() {
    const SafeInsets safe = safeInsets();
    if (layoutWidth == rendering::window_width &&
        layoutHeight == rendering::window_height &&
        layoutSafe.top == safe.top && layoutSafe.left == safe.left &&
        layoutSafe.bottom == safe.bottom && layoutSafe.right == safe.right) {
      return;
    }
    layoutWidth = rendering::window_width;
    layoutHeight = rendering::window_height;
    layoutSafe = safe;
    const auto geometry =
        layoutIrRankingPanel({.viewportWidth = layoutWidth,
                              .viewportHeight = layoutHeight,
                              .safeTop = safe.top,
                              .safeLeft = safe.left,
                              .safeBottom = safe.bottom,
                              .safeRight = safe.right,
                              .margin = kPanelMargin,
                              .maximumWidth = kPanelMaximumWidth,
                              .maximumHeight = kPanelMaximumHeight});
    root->setSize(layoutWidth, layoutHeight);
    root->setPadding(Edge::Top, safe.top + kPanelMargin);
    root->setPadding(Edge::Bottom, safe.bottom + kPanelMargin);
    root->setPadding(Edge::Left, safe.left + kPanelMargin);
    root->setPadding(Edge::Right, safe.right + kPanelMargin);
    panel->setWidth(static_cast<float>(geometry.width));
    panel->setHeight(static_cast<float>(geometry.height));
    list->itemHeight = geometry.compact ? 92 : 74;
    fetchedAt->setDisplay(geometry.compact ? YGDisplayNone : YGDisplayFlex);
    root->applyYogaLayout();
    syncRankingHeader();
    root->applyYogaLayout();
    list->rebindVisibleItems();
    if (scoreDetailOpen) {
      updateScoreDetailLayout(safe);
    }
  }

  void refreshPresentation() {
    const auto &presentation = model.presentation();
    chartTitle->setText(presentation.chartTitle);
    fetchedAt->setText(presentation.fetchedAtText.empty()
                           ? ""
                           : "Fetched " + presentation.fetchedAtText);
    refreshButton->setEnabled(presentation.canRefresh);
    comparisonCard->setVisible(presentation.comparison.has_value());
    comparisonCard->setDisplay(presentation.comparison ? YGDisplayFlex
                                                       : YGDisplayNone);
    if (presentation.comparison) {
      comparison->setText(comparisonText(*presentation.comparison));
    }

    const bool showList = presentation.state == IrRankingModalState::Success;
    status->setVisible(!showList);
    status->setDisplay(showList ? YGDisplayNone : YGDisplayFlex);
    status->setText(presentation.statusText);
    const bool showDetail = !showList && !presentation.detailText.empty();
    detail->setVisible(showDetail);
    detail->setDisplay(showDetail ? YGDisplayFlex : YGDisplayNone);
    detail->setText(presentation.detailText);
    retryButton->setVisible(presentation.canRetry);
    retryButton->setDisplay(presentation.canRetry ? YGDisplayFlex
                                                  : YGDisplayNone);
    retryButton->setEnabled(presentation.canRetry);
    rankingTable->setVisible(showList);
    rankingTable->setDisplay(showList ? YGDisplayFlex : YGDisplayNone);
    tableHeader->setVisible(showList);
    list->setVisible(showList);
    const bool showPaginationStatus =
        showList && !presentation.paginationStatusText.empty();
    paginationStatus->setText(presentation.paginationStatusText);
    paginationStatus->setVisible(showPaginationStatus);
    paginationStatus->setDisplay(showPaginationStatus ? YGDisplayFlex
                                                      : YGDisplayNone);

    if (showList && presentation.ranking != visibleRanking) {
      const bool preserveScroll = visibleRanking != nullptr;
      visibleRanking = presentation.ranking;
      const auto retained = visibleRanking;
      auto provider = [retained](int index) -> const IrChartRankingEntry & {
        return retained->entries[static_cast<std::size_t>(index)];
      };
      if (preserveScroll) {
        list->updateItemProvider(static_cast<int>(retained->entries.size()),
                                 std::move(provider));
      } else {
        list->setItemProvider(static_cast<int>(retained->entries.size()),
                              std::move(provider));
      }
    } else if (!showList) {
      visibleRanking.reset();
      list->clear();
    } else {
      list->rebindVisibleItems();
    }
    root->applyYogaLayout();
    if (showList) {
      syncRankingHeader();
      root->applyYogaLayout();
      list->rebindVisibleItems();
    }
  }

  void openRequest(IrRankingRequest request, std::string title) {
    closeNow();
    const std::uint64_t generation = service.open(request);
    const IrRankingSnapshot opened = service.snapshot();
    if (opened.generation == generation && opened.request) {
      request = *opened.request;
    } else {
      request.generation = generation;
    }
    model.open(std::move(request), std::move(title));
    open = true;
    closeRequested = false;
    root->setVisible(true);
    updateLayout();
    refreshPresentation();
    portal.present(root);
  }

  void refresh() {
    if (!open || !model.expectedRequest()) {
      return;
    }
    hideScoreDetails();
    IrRankingRequest request = *model.expectedRequest();
    const std::uint64_t generation = service.refresh(request);
    model.refresh(generation);
    refreshPresentation();
  }

  void update() {
    if (!open) {
      return;
    }
    if (closeRequested) {
      closeNow();
      return;
    }
    updateLayout();
    if (model.apply(service.snapshot())) {
      refreshPresentation();
    }
    const auto &presentation = model.presentation();
    if (presentation.canLoadNextPage && presentation.ranking &&
        shouldLoadNextIrRankingPage(
            presentation.entryCount, list->scrollOffset,
            static_cast<float>(list->getContentHeight()), list->itemHeight)) {
      (void)service.loadNextPage(presentation.generation);
    }
  }

  void closeNow() {
    hideScoreDetails();
    if (!open) {
      closeRequested = false;
      return;
    }
    service.close(model.presentation().generation);
    portal.dismiss(root);
    root->setVisible(false);
    open = false;
    closeRequested = false;
    visibleRanking.reset();
    list->clear();
  }
};

IrRankingModal::IrRankingModal(OverlayPortal &portal, IrRankingService &service)
    : impl_(std::make_unique<Impl>(portal, service)) {}

IrRankingModal::~IrRankingModal() = default;

void IrRankingModal::open(IrRankingRequest request, std::string chartTitle) {
  impl_->openRequest(std::move(request), std::move(chartTitle));
}

void IrRankingModal::update() { impl_->update(); }

void IrRankingModal::close() { impl_->closeNow(); }

bool IrRankingModal::isOpen() const { return impl_->open; }

} // namespace ir
