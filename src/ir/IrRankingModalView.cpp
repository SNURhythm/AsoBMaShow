#include "IrRankingModal.h"

#include "IrRankingService.h"
#include "../rendering/common.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/ClearLampColors.h"
#include "../view/OverlayPortal.h"
#include "../view/RecyclerView.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace ir {
namespace {

constexpr const char *kFont = "assets/fonts/notosanscjkjp.ttf";
constexpr int kPanelMaximumWidth = 1180;
constexpr int kPanelMaximumHeight = 840;
constexpr int kPanelMargin = 24;

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

bool eventPoint(const SDL_Event &event, float &x, float &y) {
  if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
    int screenX = static_cast<int>(event.button.x * rendering::widthScale);
    int screenY = static_cast<int>(event.button.y * rendering::heightScale);
    int uiX = 0;
    int uiY = 0;
    rendering::screenToUi(screenX, screenY, uiX, uiY);
    x = static_cast<float>(uiX);
    y = static_cast<float>(uiY);
    return true;
  }
  if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERUP) {
    rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
    return true;
  }
  return false;
}

class ModalScrim final : public View {
public:
  ModalScrim(View *panel, std::function<void()> requestClose)
      : panel_(panel), requestClose_(std::move(requestClose)) {}

private:
  bool handleEventsImpl(SDL_Event &event) override {
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
        (event.key.keysym.sym == SDLK_ESCAPE ||
         event.key.keysym.sym == SDLK_AC_BACK)) {
      requestClose_();
      return false;
    }
    if (event.type == SDL_MOUSEBUTTONDOWN &&
        event.button.button != SDL_BUTTON_LEFT) {
      return false;
    }
    if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN) {
      float x = 0.0f;
      float y = 0.0f;
      if (eventPoint(event, x, y) &&
          (x < panel_->getX() || x > panel_->getX() + panel_->getWidth() ||
           y < panel_->getY() || y > panel_->getY() + panel_->getHeight())) {
        requestClose_();
      }
    }
    return false;
  }

  View *panel_;
  std::function<void()> requestClose_;
};

class RankingRowView final : public View {
public:
  RankingRowView() {
    setFlexDirection(FlexDirection::Column);
    setAlignItems(YGAlignStretch);
    setPadding(Edge::Left, 12);
    setPadding(Edge::Right, 12);
    setPadding(Edge::Top, 5);
    setPadding(Edge::Bottom, 5);
    setGap(2);
    setCornerRadius(ui_theme::controlRadius());
    setBorderWidth(1);

    primary_ = new View();
    primary_->setFlexDirection(FlexDirection::Row);
    primary_->setAlignItems(YGAlignCenter);
    primary_->setGap(10);
    primary_->setFlex(1);

    rank_ = makeText(18, TextView::CENTER);
    rank_->setWidth(58);
    player_ = makeText(19);
    player_->setFlex(1);
    score_ = makeText(17, TextView::RIGHT);
    score_->setWidth(152);
    rate_ = makeText(17, TextView::RIGHT);
    rate_->setWidth(86);
    lamp_ = makeText(15, TextView::CENTER);
    lamp_->setWidth(174)->setHeight(32);
    lamp_->setCornerRadius(6);
    badPoints_ = makeText(17, TextView::RIGHT);
    badPoints_->setWidth(62);
    combo_ = makeText(17, TextView::RIGHT);
    combo_->setWidth(88);
    time_ = makeText(15, TextView::RIGHT);
    time_->setWidth(172);
    detail_ = makeText(15);
    detail_->setThemedColor(ui_theme::textSecondary);
    detail_->setHeight(26);

    primary_->addView(rank_);
    primary_->addView(player_);
    primary_->addView(score_);
    primary_->addView(rate_);
    primary_->addView(lamp_);
    primary_->addView(badPoints_);
    primary_->addView(combo_);
    primary_->addView(time_);
    addView(primary_);
    addView(detail_);
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
    detail_->setText(row.detailText);

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
    detail_->setDisplay(row.compact && row.expanded ? YGDisplayFlex
                                                    : YGDisplayNone);
    detail_->setVisible(row.compact && row.expanded);
    lamp_->setWidth(row.compact ? 144.0f : 174.0f);
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
  TextView *detail_ = nullptr;
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
  RecyclerView<IrChartRankingEntry> *list = nullptr;
  std::shared_ptr<const IrChartRanking> visibleRanking;
  bool open = false;
  bool closeRequested = false;
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
  }

  void requestClose() {
    closeRequested = true;
    if (root != nullptr) {
      root->setVisible(false);
    }
  }

  void build() {
    panel = new View();
    root = new ModalScrim(panel, [this]() { requestClose(); });
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
    closeButton = makeActionButton("Close", 96, [this]() { requestClose(); });
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
      model.toggleExpanded(index);
      list->rebindVisibleItems();
    };
    panel->addView(list);
    root->addView(panel);
    root->setVisible(false);
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
    list->rebindVisibleItems();
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
    list->setVisible(showList);
    list->setDisplay(showList ? YGDisplayFlex : YGDisplayNone);

    if (showList && presentation.ranking != visibleRanking) {
      visibleRanking = presentation.ranking;
      const auto retained = visibleRanking;
      list->setItemProvider(
          static_cast<int>(retained->entries.size()),
          [retained](int index) -> const IrChartRankingEntry & {
            return retained->entries[static_cast<std::size_t>(index)];
          });
    } else if (!showList) {
      visibleRanking.reset();
      list->clear();
    } else {
      list->rebindVisibleItems();
    }
    root->applyYogaLayout();
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
  }

  void closeNow() {
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
