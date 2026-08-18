#include "DefaultSkin.h"

#include "../scene/ResultLayoutGeometry.h"
#include "../scene/ResultPresentationModel.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
struct BpmTextPart {
  std::string prefix;
  std::string value;
};

struct BpmTextParts {
  BpmTextPart minimum;
  BpmTextPart maximum;
  BpmTextPart main;
  bool variable = false;
};

BpmTextPart splitBpmPart(std::string_view text) {
  if (text.find('.') == std::string_view::npos && text.size() > 3) {
    return {.prefix = std::string(text.substr(0, text.size() - 3)),
            .value = std::string(text.substr(text.size() - 3))};
  }
  return {.value = std::string(text)};
}

BpmTextParts splitBpmText(std::string_view text) {
  const std::size_t separator = text.find('-');
  const std::size_t open = text.find('(', separator);
  const std::size_t close = text.find(')', open);
  if (separator == std::string_view::npos || open == std::string_view::npos ||
      close == std::string_view::npos || separator >= open) {
    return {.main = splitBpmPart(text)};
  }
  return {
      .minimum = splitBpmPart(text.substr(0, separator)),
      .maximum = splitBpmPart(text.substr(separator + 1, open - separator - 1)),
      .main = splitBpmPart(text.substr(open + 1, close - open - 1)),
      .variable = true,
  };
}

std::string semanticId(std::string_view label) {
  std::string result;
  bool pendingSeparator = false;
  for (const unsigned char character : label) {
    if (std::isalnum(character) != 0) {
      if (pendingSeparator && !result.empty()) {
        result.push_back('-');
      }
      result.push_back(static_cast<char>(std::tolower(character)));
      pendingSeparator = false;
    } else {
      pendingSeparator = true;
    }
  }
  return result;
}

std::string judgementId(std::string_view label) {
  std::string result;
  for (const unsigned char character : label) {
    if (std::isalnum(character) != 0) {
      result.push_back(static_cast<char>(std::tolower(character)));
    }
  }
  return result;
}

Color comparisonDeltaAccent(const ResultComparisonCard &card) {
  if (!card.delta.has_value() || card.delta->find("--") != std::string::npos) {
    return ui_theme::textMuted();
  }
  if (card.title.find("COMBO") != std::string::npos) {
    int comboDelta = 0;
    int breakDelta = 0;
    if (std::sscanf(card.delta->c_str(), "COMBO %d / BREAK %d", &comboDelta,
                    &breakDelta) == 2) {
      return comboDelta >= 0 && breakDelta <= 0 ? ui_theme::lime()
                                                : ui_theme::amber();
    }
    return ui_theme::amber();
  }
  return card.delta->find('-') == std::string::npos ? ui_theme::lime()
                                                    : ui_theme::coral();
}

std::optional<ResultGradeCard>
legacyGradeCard(const ResultPresentationModel &presentation) {
  if (auto grade = gradeCard(presentation)) {
    return grade;
  }
  if (!presentation.score.has_value() || !presentation.maxScore.has_value()) {
    return std::nullopt;
  }
  return ResultGradeCard{
      .grade = {},
      .rate = "0.00%",
      .accent = ui_theme::scoreRankColor({}),
  };
}
} // namespace

void DefaultSkin::buildLayout(const std::string &screenName, View *root,
                              void *data) {
  if (screenName == "Result") {
    buildResultLayout(root, static_cast<ResultSkinData *>(data));
  }
}

bool DefaultSkin::rebuildLayoutSection(const std::string &sectionName,
                                       View *root, void *data) {
  if (sectionName != "ResultSummary" || root == nullptr || data == nullptr) {
    return false;
  }
  View::LayoutBatchScope layoutBatch;
  root->clearChildren();
  buildResultSummary(root, static_cast<ResultSkinData *>(data));
  return true;
}

void DefaultSkin::buildResultSummary(View *root, ResultSkinData *data) {
  buildResultLayout(root, data, true);
}

void DefaultSkin::buildResultLayout(View *rootLayout, ResultSkinData *data,
                                    bool summaryOnly) {
  if (rootLayout == nullptr || data == nullptr) {
    return;
  }

  if (data != nullptr && data->presentation != nullptr) {
    buildPresentationResultLayout(rootLayout, data, *data->presentation,
                                  summaryOnly, true);
    return;
  }

  ResultLocalPresentationOptions options{
      .playModeLabel = data->playModeLabel,
      .laneOrderLabel = data->laneOrderLabel,
      .difficultyLabel = data->difficultyLabel,
      .headerDifficultyLabelOverride = data->headerDifficultyLabelOverride,
      .currentClearLabelOverride = data->currentClearLabelOverride,
      .currentClearRankOverride = data->currentClearRankOverride,
      .previousBest = data->previousBest,
      .previousLampBest = data->previousLampBest,
      .pacemaker = data->pacemaker,
  };
  ResultPresentationModel localPresentation = makeLocalResultPresentation(
      *data->meta, *data->state, std::move(options));
  // The legacy result header did not render key mode. Keep the null-pointer
  // compatibility path pixel-compatible until callers supply the model.
  localPresentation.playtype.reset();
  buildPresentationResultLayout(rootLayout, data, localPresentation,
                                summaryOnly, false);
}

void DefaultSkin::buildPresentationResultLayout(
    View *rootLayout, ResultSkinData *data,
    const ResultPresentationModel &presentation, bool summaryOnly,
    bool authoritativePresentation) {
  const bool mobileTarget =
      TARGET_PLATFORM == iOS || TARGET_PLATFORM == Android;
  const auto layoutMetrics = result_layout::metricsFor(
      static_cast<float>(rendering::window_height), mobileTarget);

  if (!summaryOnly) {
    rootLayout->setFlexDirection(FlexDirection::Column);
    rootLayout->setAlignItems(YGAlignStretch);
    rootLayout->setJustifyContent(YGJustifyCenter);
    rootLayout->setPadding(Edge::All, layoutMetrics.rootPadding);
    rootLayout->setGap(layoutMetrics.rootGap);
    rootLayout->setBackgroundColor(ui_theme::resultBackdrop());
  }

  const auto makeLabel = [](const std::string &text, int size, Color color) {
    auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", size);
    label->setText(text);
    label->setColor(ui_theme::sdl(color));
    label->setVAlign(TextView::MIDDLE);
    return label;
  };

  const auto makePanel = [](Color fill, Color border) {
    auto *panel = new View();
    panel->setBackgroundColor(fill);
    panel->setCornerRadius(ui_theme::panelRadius());
    panel->setShadow(ui_theme::cardShadow(), ui_theme::kCardShadow);
    panel->setBorderColor(border);
    panel->setBorderWidth(1);
    return panel;
  };

  const auto makeDivider = []() {
    auto *divider = new View();
    divider->setWidth(1);
    divider->setAlignSelf(YGAlignStretch);
    divider->setBackgroundColor(ui_theme::hairlineSubtle());
    divider->setFlexShrink(0);
    return divider;
  };

  if (!summaryOnly) {
    auto *header = new View();
    header->setFlexDirection(FlexDirection::Row);
    header->setAlignItems(YGAlignCenter);
    header->setGap(20);
    header->setMinHeight(96);
    header->setName("resultHeader");

    auto *titleStack = new View();
    titleStack->setFlexDirection(FlexDirection::Column);
    titleStack->setFlex(1);
    titleStack->setGap(4);

    auto *titleRow = new View();
    titleRow->setFlexDirection(FlexDirection::Row);
    titleRow->setAlignItems(YGAlignCenter);
    titleRow->setGap(14);
    titleRow->setHeight(58);

    auto *titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 46);
    titleText->setText(presentation.title);
    titleText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
    titleText->setOverflow(TextView::TextOverflow::Marquee);
    titleText->setHeight(58);
    titleText->setFlex(1);
    titleText->setMinWidth(0);
    titleText->setName("title");
    titleRow->addView(titleText);

    if (authoritativePresentation && presentation.playtype.has_value()) {
      auto *playtypeText =
          makeLabel(*presentation.playtype, 30, ui_theme::cyan());
      playtypeText->setHeight(46);
      playtypeText->setFlexShrink(0);
      playtypeText->setWidth(
          std::min(160, std::max(1, playtypeText->textureWidth() + 12)));
      playtypeText->setAlign(TextView::CENTER);
      playtypeText->setName("playtype");
      titleRow->addView(playtypeText);
    }

    if (presentation.difficulty.has_value()) {
      auto *difficultyText = new TextView("assets/fonts/notosanscjkjp.ttf", 46);
      difficultyText->setText(*presentation.difficulty);
      difficultyText->setColor(ui_theme::sdl(ui_theme::amber()));
      difficultyText->setOverflow(TextView::TextOverflow::Hidden);
      difficultyText->setHeight(58);
      difficultyText->setFlexShrink(0);
      difficultyText->setWidth(
          std::min(440, std::max(1, difficultyText->textureWidth() + 8)));
      difficultyText->setName("difficulty");
      titleRow->addView(difficultyText);
    }
    titleStack->addView(titleRow);

    if (presentation.artist.has_value()) {
      auto *artistText =
          makeLabel(*presentation.artist, 26, ui_theme::textSecondary());
      artistText->setHeight(36);
      artistText->setName("artist");
      titleStack->addView(artistText);
    }

    header->addView(titleStack);
    rootLayout->addView(header);
  }

  const auto addPanelTitle = [&](View *panel, const std::string &title,
                                 const std::string &semanticName) {
    auto *titleView = makeLabel(title, 17, ui_theme::textSecondary());
    titleView->setHeight(25);
    titleView->setOverflow(TextView::TextOverflow::Hidden);
    titleView->setName("resultSummaryTitle:" + semanticName);
    panel->addView(titleView);
  };

  const auto configureSummaryCard = [&](View *panel,
                                        const std::string &semanticName) {
    if (authoritativePresentation) {
      panel->setWidth(0.0f);
    }
    panel->setFlexGrow(1.0f);
    panel->setFlexBasis(0.0f);
    panel->setMinWidth(0.0f);
    panel->setFlexDirection(FlexDirection::Column);
    panel->setPadding(Edge::All, layoutMetrics.summaryPanelPadding);
    panel->setGap(6);
    panel->setName("resultSummaryCard:" + semanticName);
  };

  const auto makeComparisonColumn = [&](const ResultComparisonValue &value,
                                        const std::string &semanticName,
                                        int valueSize, bool lampStyle) {
    auto *column = new View();
    column->setFlexDirection(FlexDirection::Column);
    column->setJustifyContent(YGJustifyCenter);
    column->setAlignItems(YGAlignStretch);
    column->setFlexGrow(1);
    column->setFlexBasis(0);
    column->setMinWidth(0);
    column->setGap(lampStyle ? 4 : 2);
    column->setName("resultSummaryValue:" + semanticName);

    auto *labelView = makeLabel(value.label, 16, ui_theme::textSecondary());
    labelView->setHeight(22);
    labelView->setAlign(TextView::CENTER);
    labelView->setOverflow(TextView::TextOverflow::Hidden);
    labelView->setName("resultSummaryValueLabel:" + semanticName);
    column->addView(labelView);

    if (lampStyle) {
      auto *lampRow = new View();
      lampRow->setFlexDirection(FlexDirection::Row);
      lampRow->setAlignItems(YGAlignCenter);
      lampRow->setJustifyContent(YGJustifyCenter);
      lampRow->setGap(8);
      lampRow->setHeight(48);

      auto *lampSwatch = new View();
      lampSwatch->setWidth(10);
      lampSwatch->setHeight(38);
      lampSwatch->setFlexShrink(0);
      lampSwatch->setBackgroundColor(value.accent);
      lampSwatch->setCornerRadius(4.0f);
      lampSwatch->setName("resultLampSwatch:" + semanticName);
      lampRow->addView(lampSwatch);

      auto *valueView =
          makeLabel(value.value, valueSize, ui_theme::textPrimary());
      valueView->setHeight(38);
      valueView->setFlexShrink(1);
      valueView->setMinWidth(0);
      valueView->setOverflow(TextView::TextOverflow::Hidden);
      valueView->setName("resultSummaryValueText:" + semanticName);
      lampRow->addView(valueView);
      column->addView(lampRow);
    } else {
      auto *valueView = makeLabel(value.value, valueSize, value.accent);
      valueView->setHeight(valueSize + 8);
      valueView->setAlign(TextView::CENTER);
      valueView->setOverflow(TextView::TextOverflow::Hidden);
      valueView->setName("resultSummaryValueText:" + semanticName);
      column->addView(valueView);
    }

    if (!value.detail.empty() || !authoritativePresentation) {
      auto *detailView = makeLabel(value.detail, 18, ui_theme::textSecondary());
      detailView->setHeight(24);
      detailView->setAlign(TextView::CENTER);
      detailView->setOverflow(TextView::TextOverflow::Hidden);
      detailView->setName("resultSummaryValueDetail:" + semanticName);
      column->addView(detailView);
    }
    return column;
  };

  const auto makeComparisonCard = [&](const ResultComparisonCard &card,
                                      const std::string &semanticName,
                                      bool lampStyle, int valueSize) {
    const Color border =
        lampStyle ? ui_theme::withAlpha(card.current.accent,
                                        card.target.has_value() &&
                                                card.target->value != "NO PLAY"
                                            ? 120
                                            : 96)
                  : ui_theme::hairlineSubtle();
    auto *panel = makePanel(ui_theme::resultPanel(), border);
    configureSummaryCard(panel, semanticName);
    addPanelTitle(panel, card.title, semanticName);

    auto *comparisonRow = new View();
    comparisonRow->setFlexDirection(FlexDirection::Row);
    comparisonRow->setAlignItems(YGAlignStretch);
    comparisonRow->setGap(10);
    comparisonRow->setFlexGrow(1);
    comparisonRow->setName("resultSummaryComparison:" + semanticName);
    if (card.target.has_value()) {
      comparisonRow->addView(makeComparisonColumn(
          *card.target, semanticName + ":target", valueSize, lampStyle));
      comparisonRow->addView(makeDivider());
    }
    comparisonRow->addView(makeComparisonColumn(
        card.current, semanticName + ":current", valueSize, lampStyle));
    panel->addView(comparisonRow);

    if (card.delta.has_value()) {
      auto *deltaView =
          makeLabel(*card.delta, semanticName == "combo" ? 20 : 22,
                    comparisonDeltaAccent(card));
      deltaView->setHeight(semanticName == "combo" ? 28 : 30);
      deltaView->setAlign(TextView::CENTER);
      deltaView->setOverflow(TextView::TextOverflow::Hidden);
      deltaView->setName("resultSummaryDelta:" + semanticName);
      panel->addView(deltaView);
    }
    return panel;
  };

  std::vector<View *> summaryCards;
  const auto grade = authoritativePresentation ? gradeCard(presentation)
                                               : legacyGradeCard(presentation);
  if (grade.has_value()) {
    auto *gradePanel = makePanel(
        ui_theme::resultPanelStrong(),
        ui_theme::withAlpha(grade->accent, static_cast<uint8_t>(138)));
    if (authoritativePresentation) {
      gradePanel->setWidth(0.0f);
      gradePanel->setFlexGrow(1.0f);
      gradePanel->setFlexBasis(0.0f);
      gradePanel->setMinWidth(0.0f);
    } else {
      gradePanel->setWidth(196);
      gradePanel->setFlexShrink(0);
    }
    gradePanel->setFlexDirection(FlexDirection::Column);
    gradePanel->setAlignItems(YGAlignCenter);
    gradePanel->setJustifyContent(YGJustifyCenter);
    gradePanel->setPadding(Edge::All, authoritativePresentation
                                          ? layoutMetrics.summaryPanelPadding
                                          : layoutMetrics.gradePanelPadding);
    gradePanel->setGap(2);
    gradePanel->setName("resultSummaryCard:grade");

    auto *gradeLabel = makeLabel("GRADE", 17, ui_theme::textSecondary());
    gradeLabel->setHeight(23);
    gradeLabel->setAlign(TextView::CENTER);
    gradeLabel->setName("resultSummaryTitle:grade");
    gradePanel->addView(gradeLabel);

    auto *gradeText = new TextView("assets/fonts/notosanscjkjp.ttf", 96);
    gradeText->setText(grade->grade);
    gradeText->setColor(ui_theme::sdl(grade->accent));
    gradeText->setAlign(TextView::CENTER);
    gradeText->setHeight(106);
    gradeText->setName("grade");
    gradePanel->addView(gradeText);

    auto *rateText = makeLabel(grade->rate, 22, ui_theme::cyan());
    rateText->setHeight(30);
    rateText->setAlign(TextView::CENTER);
    rateText->setName("resultGradeRate");
    gradePanel->addView(rateText);
    summaryCards.push_back(gradePanel);
  }
  if (presentation.scoreComparison.has_value()) {
    summaryCards.push_back(
        makeComparisonCard(*presentation.scoreComparison, "score", false, 44));
  }
  if (presentation.lampComparison.has_value()) {
    summaryCards.push_back(
        makeComparisonCard(*presentation.lampComparison, "lamp", true, 24));
  }
  if (presentation.comboComparison.has_value()) {
    summaryCards.push_back(
        makeComparisonCard(*presentation.comboComparison, "combo", false, 38));
  }

  if (!summaryCards.empty()) {
    auto *summaryRow = summaryOnly ? rootLayout : new View();
    summaryRow->setDisplay(YGDisplayFlex);
    summaryRow->setFlexDirection(FlexDirection::Row);
    summaryRow->setAlignItems(YGAlignStretch);
    summaryRow->setGap(12);
    summaryRow->setHeight(layoutMetrics.summaryHeight);
    summaryRow->setName("resultSummary");
    for (View *card : summaryCards) {
      summaryRow->addView(card);
    }
    if (!summaryOnly) {
      rootLayout->addView(summaryRow);
    }
  } else if (summaryOnly) {
    rootLayout->setHeight(0);
    rootLayout->setDisplay(YGDisplayNone);
  }

  if (summaryOnly) {
    return;
  }

  if (!presentation.infoTiles.empty()) {
    auto *infoGrid =
        makePanel(ui_theme::resultPanelSubtle(), ui_theme::hairlineSubtle());
    infoGrid->setFlexDirection(FlexDirection::Row);
    infoGrid->setFlexWrap(authoritativePresentation ? YGWrapWrap
                                                    : YGWrapNoWrap);
    infoGrid->setJustifyContent(YGJustifyCenter);
    infoGrid->setAlignItems(YGAlignStretch);
    if (authoritativePresentation) {
      infoGrid->setMinHeight(layoutMetrics.infoHeight);
    } else {
      infoGrid->setHeight(layoutMetrics.infoHeight);
    }
    infoGrid->setName("resultInfoGrid");

    const auto addInfoTile = [&](const ResultInfoTile &info) {
      if (!authoritativePresentation && !infoGrid->getChildren().empty()) {
        infoGrid->addView(makeDivider());
      }
      const std::string id = semanticId(info.label);
      auto *tile = new View();
      tile->setFlexGrow(1);
      tile->setFlexShrink(1);
      if (authoritativePresentation) {
        tile->setFlexBasis(176.0f);
        tile->setMinWidth(144.0f);
        tile->setHeight(layoutMetrics.infoHeight);
      } else {
        tile->setFlexBasis(0);
        tile->setMinWidth(0);
      }
      tile->setPadding(Edge::All, layoutMetrics.infoTilePadding);
      tile->setFlexDirection(FlexDirection::Column);
      tile->setJustifyContent(YGJustifyCenter);
      tile->setName("resultInfoTile:" + id);

      auto *labelView = makeLabel(info.label, 15, ui_theme::textSecondary());
      labelView->setHeight(21);
      labelView->setAlign(TextView::CENTER);
      labelView->setOverflow(TextView::TextOverflow::Hidden);
      labelView->setName("resultInfoLabel:" + id);
      tile->addView(labelView);

      if (!authoritativePresentation && info.label == "BPM") {
        const BpmTextParts bpm = splitBpmText(info.value);
        auto *valueRow = new View();
        valueRow->setFlexDirection(FlexDirection::Row);
        valueRow->setAlignItems(YGAlignCenter);
        valueRow->setJustifyContent(YGJustifyCenter);
        valueRow->setHeight(38);
        valueRow->setMinWidth(0);
        valueRow->setName("BPM");

        const int valueSize = bpm.variable ? 23 : 31;
        const int prefixSize = bpm.variable ? 14 : 18;
        const int punctuationSize = bpm.variable ? 18 : 22;
        const auto appendText = [&](const std::string &text, int size,
                                    Color color) {
          if (text.empty()) {
            return;
          }
          auto *textView = makeLabel(text, size, color);
          textView->setWidth(std::max(1, textView->textureWidth() + 1));
          textView->setHeight(38);
          textView->setAlign(TextView::CENTER);
          textView->setVAlign(TextView::MIDDLE);
          textView->setOverflow(TextView::TextOverflow::Hidden);
          textView->setFlexShrink(0);
          valueRow->addView(textView);
        };
        const auto appendPart = [&](const BpmTextPart &part) {
          appendText(part.prefix, prefixSize, ui_theme::textMuted());
          appendText(part.value, valueSize, ui_theme::textPrimary());
        };
        if (bpm.variable) {
          appendPart(bpm.minimum);
          appendText("-", punctuationSize, ui_theme::textSecondary());
          appendPart(bpm.maximum);
          appendText("(", punctuationSize, ui_theme::textSecondary());
          appendPart(bpm.main);
          appendText(")", punctuationSize, ui_theme::textSecondary());
        } else {
          appendPart(bpm.main);
        }
        tile->addView(valueRow);

        auto *detailView = makeLabel("", 18, info.accent);
        detailView->setHeight(22);
        detailView->setName("resultInfoDetail:" + id);
        tile->addView(detailView);
        infoGrid->addView(tile);
        return;
      }

      const bool laneOrder =
          info.label == "PLAY MODE" && info.detail.has_value();
      int valueSize = laneOrder ? 24 : 31;
      if (info.label == "BPM" && info.value.size() > 9) {
        valueSize = 23;
      }
      const Color valueColor =
          info.label == "PLAY MODE" && !info.detail.has_value()
              ? info.accent
              : (presentation.readOnlyIrUploaded && !info.detail.has_value()
                     ? info.accent
                     : ui_theme::textPrimary());
      auto *valueView = makeLabel(info.value, valueSize, valueColor);
      valueView->setHeight(laneOrder ? 30 : 38);
      valueView->setAlign(TextView::CENTER);
      valueView->setOverflow(TextView::TextOverflow::Hidden);
      valueView->setName(info.label);
      tile->addView(valueView);

      if (laneOrder) {
        auto *laneRow = new View();
        laneRow->setFlexDirection(FlexDirection::Row);
        laneRow->setAlignItems(YGAlignCenter);
        laneRow->setJustifyContent(YGJustifyCenter);
        laneRow->setHeight(24);
        laneRow->setName("resultInfoDetail:" + id);
        for (const char symbol : *info.detail) {
          auto *symbolView =
              makeLabel(std::string(1, symbol), 18,
                        symbol == 'S' ? ui_theme::coral() : info.accent);
          symbolView->setWidth(std::max(10, symbolView->textureWidth() + 1));
          symbolView->setHeight(24);
          symbolView->setAlign(TextView::CENTER);
          symbolView->setOverflow(TextView::TextOverflow::Hidden);
          laneRow->addView(symbolView);
        }
        tile->addView(laneRow);
      } else if (info.detail.has_value() || !authoritativePresentation) {
        auto *detailView =
            makeLabel(info.detail.value_or(std::string{}), 18, info.accent);
        detailView->setHeight(22);
        detailView->setAlign(TextView::CENTER);
        detailView->setOverflow(TextView::TextOverflow::Hidden);
        detailView->setName("resultInfoDetail:" + id);
        tile->addView(detailView);
      }
      infoGrid->addView(tile);
    };

    for (const ResultInfoTile &info : presentation.infoTiles) {
      addInfoTile(info);
    }
    rootLayout->addView(infoGrid);
  }

  const bool showJudgements = hasJudgementCard(presentation);
  const bool showBreak = presentation.comboBreak.has_value();
  const bool showFast = presentation.fast.has_value();
  const bool showSlow = presentation.slow.has_value();
  if (showJudgements || showBreak || showFast || showSlow) {
    auto *detailsGrid =
        makePanel(ui_theme::resultPanelSubtle(), ui_theme::hairlineSubtle());
    detailsGrid->setFlexDirection(FlexDirection::Row);
    detailsGrid->setFlexWrap(YGWrapNoWrap);
    detailsGrid->setJustifyContent(YGJustifyCenter);
    detailsGrid->setAlignItems(YGAlignStretch);
    detailsGrid->setHeight(layoutMetrics.detailsHeight);
    detailsGrid->setName("detailsGrid");

    const auto addSeparator = [&]() {
      if (!authoritativePresentation && !detailsGrid->getChildren().empty()) {
        detailsGrid->addView(makeDivider());
      }
    };

    const auto makeMetricTile = [&](const std::string &label, int value,
                                    Color accent, const std::string &id) {
      addSeparator();
      auto *tile = new View();
      tile->setFlexGrow(1);
      tile->setFlexBasis(0);
      tile->setFlexShrink(1);
      tile->setMinWidth(0);
      tile->setPadding(Edge::All, layoutMetrics.detailsTilePadding);
      tile->setFlexDirection(FlexDirection::Column);
      tile->setJustifyContent(YGJustifyCenter);
      tile->setName("resultMetricTile:" + id);

      auto *labelView = makeLabel(label, 15, ui_theme::textSecondary());
      labelView->setHeight(21);
      labelView->setAlign(TextView::CENTER);
      labelView->setOverflow(TextView::TextOverflow::Hidden);
      labelView->setName("resultMetricLabel:" + id);
      tile->addView(labelView);

      auto *valueView = makeLabel(std::to_string(value), 34, accent);
      valueView->setHeight(43);
      valueView->setAlign(TextView::CENTER);
      valueView->setOverflow(TextView::TextOverflow::Hidden);
      valueView->setName(id);
      tile->addView(valueView);
      detailsGrid->addView(tile);
    };

    if (showJudgements) {
      for (const ResultJudgementRow &judgement : presentation.judgements) {
        addSeparator();
        const std::string id = judgementId(judgement.label);
        auto *tile = new View();
        tile->setFlexGrow(1);
        tile->setFlexBasis(0);
        tile->setFlexShrink(1);
        tile->setMinWidth(0);
        tile->setPadding(Edge::All, layoutMetrics.detailsTilePadding);
        tile->setFlexDirection(FlexDirection::Column);
        tile->setJustifyContent(YGJustifyCenter);
        tile->setName("resultJudgementTile:" + id);

        auto *labelView =
            makeLabel(judgement.label, 15, ui_theme::textSecondary());
        labelView->setHeight(21);
        labelView->setAlign(TextView::CENTER);
        labelView->setOverflow(TextView::TextOverflow::Hidden);
        labelView->setName("resultJudgementLabel:" + id);
        tile->addView(labelView);

        auto *valueView =
            makeLabel(std::to_string(judgement.total), 34, judgement.color);
        valueView->setHeight(40);
        valueView->setAlign(TextView::CENTER);
        valueView->setOverflow(TextView::TextOverflow::Hidden);
        valueView->setName(id);
        tile->addView(valueView);

        if (judgement.early.has_value() && judgement.late.has_value()) {
          auto *timingRow = new View();
          timingRow->setFlexDirection(FlexDirection::Row);
          timingRow->setAlignItems(YGAlignCenter);
          timingRow->setJustifyContent(YGJustifyCenter);
          timingRow->setHeight(24);
          timingRow->setName("resultJudgementTiming:" + id);

          auto *fastText = makeLabel(std::to_string(*judgement.early), 20,
                                     ui_theme::fastFeedback());
          fastText->setWidth(52);
          fastText->setHeight(24);
          fastText->setAlign(TextView::RIGHT);
          fastText->setName(id + "Fast");
          timingRow->addView(fastText);

          auto *slashText = makeLabel("/", 20, ui_theme::textSecondary());
          slashText->setWidth(14);
          slashText->setHeight(24);
          slashText->setAlign(TextView::CENTER);
          timingRow->addView(slashText);

          auto *slowText = makeLabel(std::to_string(*judgement.late), 20,
                                     ui_theme::slowFeedback());
          slowText->setWidth(52);
          slowText->setHeight(24);
          slowText->setAlign(TextView::LEFT);
          slowText->setName(id + "Slow");
          timingRow->addView(slowText);
          tile->addView(timingRow);
        }
        detailsGrid->addView(tile);
      }
    }
    if (showBreak) {
      makeMetricTile("BREAK", *presentation.comboBreak, ui_theme::coral(),
                     "break");
    }
    if (showFast) {
      makeMetricTile("FAST", *presentation.fast, ui_theme::fastFeedback(),
                     "fast");
    }
    if (showSlow) {
      makeMetricTile("SLOW", *presentation.slow, ui_theme::slowFeedback(),
                     "slow");
    }
    rootLayout->addView(detailsGrid);
  }

  const bool showGraph = data->showResultGraph && (!authoritativePresentation ||
                                                   hasGaugeCard(presentation));
  View *graphPlaceHolder = nullptr;
  if (showGraph) {
    graphPlaceHolder = new Button();
    graphPlaceHolder->setBackgroundColor(ui_theme::resultPanelSubtle());
    graphPlaceHolder->setCornerRadius(ui_theme::panelRadius());
    graphPlaceHolder->setShadow(ui_theme::cardShadow(), ui_theme::kCardShadow);
    graphPlaceHolder->setBorderColor(ui_theme::hairlineSubtle());
    graphPlaceHolder->setBorderWidth(1);
    graphPlaceHolder->setName("graph");
  }

  if (data->outGraphPlaceholder != nullptr) {
    *data->outGraphPlaceholder = graphPlaceHolder;
  }

  const bool showTimingAnalytics =
      authoritativePresentation ? presentation.timingAnalytics.has_value()
                                : data->showTimingAnalytics;
  if (graphPlaceHolder != nullptr && data->showControls &&
      showTimingAnalytics) {
    auto *visualsRow = new View();
    visualsRow->setName("resultVisuals");
    visualsRow->setWidthPercent(100.0f);
    visualsRow->setHeight(layoutMetrics.visualHeight);
    visualsRow->setMinHeight(layoutMetrics.visualMinimumHeight);
    visualsRow->setFlexShrink(1.0f);
    visualsRow->setFlexDirection(FlexDirection::Row);
    visualsRow->setAlignItems(YGAlignStretch);
    visualsRow->setGap(layoutMetrics.visualGap);

    graphPlaceHolder->setFlexGrow(layoutMetrics.graphFlex);
    graphPlaceHolder->setFlexBasis(0.0f);
    graphPlaceHolder->setMinWidth(0.0f);
    visualsRow->addView(graphPlaceHolder);

    auto *timingAnalyticsHost = new View();
    timingAnalyticsHost->setName("timingAnalytics");
    timingAnalyticsHost->setFlexGrow(layoutMetrics.analyticsFlex);
    timingAnalyticsHost->setFlexBasis(0.0f);
    timingAnalyticsHost->setMinWidth(0.0f);
    timingAnalyticsHost->setFlexDirection(FlexDirection::Column);
    timingAnalyticsHost->setAlignItems(YGAlignStretch);
    visualsRow->addView(timingAnalyticsHost);
    rootLayout->addView(visualsRow);
  } else if (graphPlaceHolder != nullptr) {
    graphPlaceHolder->setHeight(result_layout::kLegacyGraphHeight);
    graphPlaceHolder->setWidthPercent(100.0f);
    rootLayout->addView(graphPlaceHolder);
  }

  if (!data->showControls) {
    return;
  }

  auto *actionsRow = new View();
  actionsRow->setFlexDirection(FlexDirection::Row);
  actionsRow->setAlignItems(YGAlignCenter);
  actionsRow->setJustifyContent(YGJustifyCenter);
  actionsRow->setFlexWrap(YGWrapWrap);
  actionsRow->setGap(14);
  actionsRow->setMinHeight(result_layout::kActionHeight);
  actionsRow->setFlexShrink(0.0f);
  actionsRow->setName("resultActions");

  auto *button = new Button(0, 0, 232, 64);
  auto *buttonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  buttonText->setText("Back to Menu");
  buttonText->setAlign(TextView::CENTER);
  buttonText->setVAlign(TextView::MIDDLE);
  buttonText->setColor(
      ui_theme::sdl(ui_theme::textOn(ui_theme::primaryAction())));
  button->setContentView(buttonText);
  button->setName("backButton");
  button->setSize(232, 64);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setBackgroundColors(
      ui_theme::withAlpha(ui_theme::primaryAction(), 182),
      ui_theme::withAlpha(ui_theme::primaryActionHover(), 210),
      ui_theme::withAlpha(ui_theme::primaryActionPressed(), 226));
  button->setBorderColors(ui_theme::withAlpha(ui_theme::cyan(), 170),
                          ui_theme::withAlpha(ui_theme::cyan(), 210),
                          Color(255, 255, 255, 216));
  button->setStyledBorderWidth(1);
  ApplicationContext *context = data->context;
  button->setOnClickListener([context]() {
    if (context != nullptr && context->sceneManager != nullptr) {
      context->sceneManager->changeScene("MainMenu");
    }
  });
  actionsRow->addView(button);
  rootLayout->addView(actionsRow);
}

void DefaultSkin::buildGameContext(View *root, void *data) {
  (void)root;
  (void)data;
}
