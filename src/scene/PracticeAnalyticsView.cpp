#include "PracticeAnalyticsView.h"
#include "PracticeAnalyticsPresentation.h"

#include "../rendering/SimpleBatchRenderer.h"
#include "../rendering/common.h"
#include "../view/Button.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace {
constexpr const char *kFont = "assets/fonts/notosanscjkjp.ttf";

TextView *makeText(std::string text, int size, Color color) {
  auto *view = new TextView(kFont, size);
  view->setText(text);
  view->setColor(ui_theme::sdl(color));
  view->setVAlign(TextView::MIDDLE);
  view->setOverflow(TextView::TextOverflow::Hidden);
  return view;
}

Button *makeButton(std::string label, int width = 120) {
  auto *button = new Button(0, 0, width, 44);
  button->setWidth(width);
  button->setHeight(44);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setStyledBorderWidth(1);
  button->setBackgroundColors(ui_theme::control(), ui_theme::controlHover(),
                              ui_theme::controlPressed());
  button->setBorderColors(ui_theme::hairline(), ui_theme::cyan(),
                          ui_theme::cyan());
  auto *text = makeText(std::move(label), 15, ui_theme::textPrimary());
  text->setAlign(TextView::CENTER);
  button->setContentView(text);
  return button;
}

std::string formatMetric(const std::optional<double> &value) {
  if (!value.has_value()) {
    return "--";
  }
  std::ostringstream stream;
  stream << std::showpos << std::fixed << std::setprecision(1) << *value
         << " ms";
  return stream.str();
}

std::string modeName(PracticeAnalyticsMode mode) {
  switch (mode) {
  case PracticeAnalyticsMode::Histogram:
    return "Histogram";
  case PracticeAnalyticsMode::Lanes:
    return "Lanes";
  case PracticeAnalyticsMode::Sections:
    return "Sections";
  }
  return "Histogram";
}

bool pointInside(const View &view, float x, float y) {
  return x >= view.getX() && x <= view.getX() + view.getWidth() &&
         y >= view.getY() && y <= view.getY() + view.getHeight();
}

void mouseToUi(int rawX, int rawY, float &x, float &y) {
  rendering::screenToUi(rawX * rendering::widthScale,
                        rawY * rendering::heightScale, x, y);
}

class AnalyticsChartView final : public View {
public:
  AnalyticsChartView(practice::ResultModel &model,
                     std::function<void(std::size_t, std::size_t)> listener)
      : model(model), listener(std::move(listener)) {
    batch.setSubmitView(rendering::ui_view);
  }

  void setMode(PracticeAnalyticsMode value) {
    mode = value;
    pointerCapture.cancelAll();
  }

protected:
  void renderImpl(RenderContext &context) override {
    const int width = getWidth();
    const int height = getHeight();
    if (width <= 0 || height <= 0) {
      return;
    }
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    batch.begin();
    batch.addRect(static_cast<float>(getX()), static_cast<float>(getY()),
                  static_cast<float>(width), static_cast<float>(height),
                  ui_theme::resultPanelSubtle().toABGR());
    switch (mode) {
    case PracticeAnalyticsMode::Histogram:
      renderHistogram();
      break;
    case PracticeAnalyticsMode::Lanes:
      renderLanes();
      break;
    case PracticeAnalyticsMode::Sections:
      renderSections();
      break;
    }
    batch.end();
  }

  bool handleEventsImpl(SDL_Event &event) override {
    if (mode != PracticeAnalyticsMode::Sections ||
        model.displayedAnalysis().sections.empty()) {
      pointerCapture.cancelAll();
      return true;
    }

    using practice_analytics_presentation::PointerPhase;
    using practice_analytics_presentation::PointerTransition;
    float x = 0.0f;
    float y = 0.0f;
    switch (event.type) {
    case SDL_MOUSEBUTTONDOWN:
      if (event.button.button != SDL_BUTTON_LEFT) {
        return true;
      }
      mouseToUi(event.button.x, event.button.y, x, y);
      if (!pointInside(*this, x, y)) {
        return true;
      }
      if (pointerCapture.handleMouse(PointerPhase::Down,
                                     event.button.which == SDL_TOUCH_MOUSEID) !=
          PointerTransition::Begin) {
        return true;
      }
      mouseDragFirst = sectionForX(x);
      publish(mouseDragFirst, mouseDragFirst);
      return false;
    case SDL_MOUSEMOTION:
      if (pointerCapture.handleMouse(PointerPhase::Move,
                                     event.motion.which == SDL_TOUCH_MOUSEID) !=
          PointerTransition::Update) {
        return true;
      }
      mouseToUi(event.motion.x, event.motion.y, x, y);
      publish(mouseDragFirst, sectionForX(x));
      return false;
    case SDL_MOUSEBUTTONUP:
      if (event.button.button != SDL_BUTTON_LEFT ||
          pointerCapture.handleMouse(PointerPhase::Up,
                                     event.button.which == SDL_TOUCH_MOUSEID) !=
              PointerTransition::End) {
        return true;
      }
      mouseToUi(event.button.x, event.button.y, x, y);
      publish(mouseDragFirst, sectionForX(x));
      return false;
    case SDL_FINGERDOWN:
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
      if (!pointInside(*this, x, y)) {
        return true;
      }
      if (pointerCapture.handleTouch(PointerPhase::Down,
                                     event.tfinger.fingerId) !=
          PointerTransition::Begin) {
        return true;
      }
      touchDragFirst = sectionForX(x);
      publish(touchDragFirst, touchDragFirst);
      return false;
    case SDL_FINGERMOTION:
      if (pointerCapture.handleTouch(PointerPhase::Move,
                                     event.tfinger.fingerId) !=
          PointerTransition::Update) {
        return true;
      }
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
      publish(touchDragFirst, sectionForX(x));
      return false;
    case SDL_FINGERUP:
      if (pointerCapture.handleTouch(PointerPhase::Up,
                                     event.tfinger.fingerId) !=
          PointerTransition::End) {
        return true;
      }
      rendering::normalizedToUi(event.tfinger.x, event.tfinger.y, x, y);
      publish(touchDragFirst, sectionForX(x));
      return false;
    case SDL_WINDOWEVENT:
      if (event.window.event == SDL_WINDOWEVENT_LEAVE ||
          event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        pointerCapture.cancelAll();
      }
      return true;
    default:
      return true;
    }
  }

private:
  practice::ResultModel &model;
  std::function<void(std::size_t, std::size_t)> listener;
  PracticeAnalyticsMode mode = PracticeAnalyticsMode::Histogram;
  rendering::SimpleBatchRenderer batch;
  practice_analytics_presentation::PointerCaptureState pointerCapture;
  std::size_t mouseDragFirst = 0;
  std::size_t touchDragFirst = 0;

  std::size_t sectionForX(float x) const {
    const std::size_t count = model.displayedAnalysis().sections.size();
    if (count <= 1 || getWidth() <= 0) {
      return 0;
    }
    return practice_analytics_presentation::exactSectionForX(
        count, x - static_cast<float>(getX()), static_cast<float>(getWidth()));
  }

  void publish(std::size_t first, std::size_t last) {
    if (listener) {
      listener(first, last);
    }
  }

  void renderHistogram() {
    const auto &analysis = model.displayedAnalysis();
    const float x = static_cast<float>(getX());
    const float y = static_cast<float>(getY());
    const float width = static_cast<float>(getWidth());
    const float height = static_cast<float>(getHeight());
    int extent = 25;
    std::size_t maximumCount = 1;
    for (const auto &bin : analysis.histogram) {
      extent = std::max(extent, std::max(std::abs(bin.lowerMillis),
                                         std::abs(bin.upperMillis)));
      maximumCount = std::max(maximumCount, bin.count);
    }
    const float center = x + width * 0.5f;
    batch.addLine(center, y + 4.0f, center, y + height - 4.0f, 2.0f,
                  ui_theme::textSecondary().toABGR());
    for (const auto &bin : analysis.histogram) {
      const float x0 =
          center + static_cast<float>(bin.lowerMillis) / extent * width * 0.5f;
      const float x1 =
          center + static_cast<float>(bin.upperMillis) / extent * width * 0.5f;
      const float barHeight = (height - 10.0f) * static_cast<float>(bin.count) /
                              static_cast<float>(maximumCount);
      batch.addRect(
          std::min(x0, x1), y + height - barHeight - 4.0f,
          std::max(1.0f, std::abs(x1 - x0) - 1.0f), barHeight,
          (bin.upperMillis <= 0 ? ui_theme::cyan() : ui_theme::amber())
              .toABGR());
    }
    if (analysis.histogramLowerOverflow > 0) {
      batch.addRect(x, y + 4.0f, 5.0f, height - 8.0f,
                    ui_theme::coral().toABGR());
    }
    if (analysis.histogramUpperOverflow > 0) {
      batch.addRect(x + width - 5.0f, y + 4.0f, 5.0f, height - 8.0f,
                    ui_theme::coral().toABGR());
    }
  }

  void renderLanes() {
    const auto &lanes = model.displayedAnalysis().lanes;
    const float x = static_cast<float>(getX());
    const float y = static_cast<float>(getY());
    const float width = static_cast<float>(getWidth());
    const float height = static_cast<float>(getHeight());
    const float center = x + width * 0.5f;
    batch.addLine(center, y + 2.0f, center, y + height - 2.0f, 2.0f,
                  ui_theme::textSecondary().toABGR());
    if (lanes.empty()) {
      return;
    }
    double extent = 10.0;
    for (const auto &lane : lanes) {
      if (lane.timing.meanMillis.has_value()) {
        extent = std::max(extent, std::abs(*lane.timing.meanMillis));
      }
    }
    const float rowHeight = height / static_cast<float>(lanes.size());
    for (std::size_t index = 0; index < lanes.size(); ++index) {
      const float rowY = y + rowHeight * index;
      batch.addLine(x, rowY + rowHeight, x + width, rowY + rowHeight, 1.0f,
                    ui_theme::hairlineSubtle().toABGR());
      if (!lanes[index].timing.meanMillis.has_value()) {
        continue;
      }
      const double mean = *lanes[index].timing.meanMillis;
      const float end =
          center + static_cast<float>(mean / extent) * (width * 0.46f);
      batch.addRoundedRect(
          std::min(center, end), rowY + 4.0f,
          std::max(2.0f, std::abs(end - center)),
          std::max(3.0f, rowHeight - 8.0f), 3.0f,
          (mean < 0.0 ? ui_theme::cyan() : ui_theme::amber()).toABGR());
    }
  }

  static Color sectionColor(practice_analytics_presentation::SectionTone tone) {
    using practice_analytics_presentation::SectionTone;
    if (tone == SectionTone::Danger) {
      return ui_theme::coral();
    }
    if (tone == SectionTone::Neutral) {
      return ui_theme::control();
    }
    if (tone == SectionTone::Early) {
      return ui_theme::cyan();
    }
    if (tone == SectionTone::Late) {
      return ui_theme::amber();
    }
    return ui_theme::lime();
  }

  void renderSections() {
    const auto &sections = model.displayedAnalysis().sections;
    if (sections.empty()) {
      return;
    }
    const float x = static_cast<float>(getX());
    const float y = static_cast<float>(getY());
    const float width = static_cast<float>(getWidth());
    const float height = static_cast<float>(getHeight());
    const float exactWidth = width / static_cast<float>(sections.size());
    const auto visualGroups =
        practice_analytics_presentation::visualSectionGroups(sections, width,
                                                             6.0f);
    for (const auto &group : visualGroups) {
      const float groupX =
          x + exactWidth * static_cast<float>(group.firstSection);
      const float groupEnd =
          x + exactWidth * static_cast<float>(group.lastSection + 1);
      batch.addRect(groupX, y + 2.0f, std::max(1.0f, groupEnd - groupX - 1.0f),
                    height - 4.0f, sectionColor(group.tone).toABGR());
    }
    if (const auto selected = model.selectedRange(); selected.has_value()) {
      const auto first = std::find_if(
          sections.begin(), sections.end(), [&](const auto &section) {
            return section.startMicros == selected->startMicros;
          });
      const auto last = std::find_if(
          sections.begin(), sections.end(), [&](const auto &section) {
            return section.endMicros == selected->endMicros;
          });
      if (first != sections.end() && last != sections.end()) {
        const std::size_t firstIndex =
            static_cast<std::size_t>(std::distance(sections.begin(), first));
        const std::size_t lastIndex =
            static_cast<std::size_t>(std::distance(sections.begin(), last));
        const float selectedX = x + exactWidth * firstIndex;
        const float selectedEnd = x + exactWidth * (lastIndex + 1);
        batch.addRect(selectedX, y, selectedEnd - selectedX, height,
                      Color(255, 255, 255, 70).toABGR());
        batch.addLine(selectedX, y, selectedX, y + height, 3.0f,
                      Color(255, 255, 255, 230).toABGR());
        batch.addLine(selectedEnd, y, selectedEnd, y + height, 3.0f,
                      Color(255, 255, 255, 230).toABGR());
      }
    }
  }
};

} // namespace

PracticeAnalyticsView::PracticeAnalyticsView(practice::ResultModel model)
    : model(std::move(model)) {
  for (std::size_t index = 0; index < this->model.compatibilityGroups().size();
       ++index) {
    choices.push_back({.attemptIndex = std::nullopt, .groupIndex = index});
  }
  for (std::size_t index = 0; index < this->model.completedAttempts();
       ++index) {
    choices.push_back({.attemptIndex = index});
  }
  build();
  refreshText();
}

void PracticeAnalyticsView::build() {
  setWidthPercent(100.0f);
  setFlexGrow(1.0f);
  setFlexShrink(1.0f);
  setMinWidth(0);
  setMinHeight(0);
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setGap(6);
  setPadding(Edge::All, 10);
  setBackgroundColor(ui_theme::resultPanel());
  setCornerRadius(ui_theme::panelRadius());
  setBorderColor(ui_theme::hairlineSubtle());
  setBorderWidth(1);

  modeControlsRow = new View();
  modeControlsRow->setHeight(44);
  modeControlsRow->setFlexDirection(FlexDirection::Row);
  modeControlsRow->setAlignItems(YGAlignCenter);
  modeControlsRow->setGap(8);
  for (const auto &[label, value] :
       {std::pair{"Histogram", PracticeAnalyticsMode::Histogram},
        std::pair{"Lanes", PracticeAnalyticsMode::Lanes},
        std::pair{"Sections", PracticeAnalyticsMode::Sections}}) {
    auto *button = makeButton(label, 112);
    button->setOnClickListener([this, value]() { setMode(value); });
    modeButtons.push_back(button);
    modeControlsRow->addView(button);
  }
  auto *spacer = new View();
  spacer->setFlexGrow(1.0f);
  modeControlsRow->addView(spacer);
  auto *abandoned =
      makeText("Abandoned: " + std::to_string(model.abandonedAttempts()), 14,
               model.abandonedAttempts() == 0 ? ui_theme::textMuted()
                                              : ui_theme::amber());
  abandoned->setWidth(140);
  abandoned->setHeight(28);
  abandoned->setAlign(TextView::RIGHT);
  modeControlsRow->addView(abandoned);
  addView(modeControlsRow);

  choiceRow = new View();
  choiceRow->setHeight(44);
  choiceRow->setFlexDirection(FlexDirection::Row);
  choiceRow->setAlignItems(YGAlignCenter);
  choiceRow->setGap(8);
  previousChoiceButton = makeButton("<", 48);
  previousChoiceButton->setOnClickListener([this]() { moveSelection(-1); });
  choiceRow->addView(previousChoiceButton);
  selectionText = makeText("", 15, ui_theme::textPrimary());
  selectionText->setFlexGrow(1.0f);
  selectionText->setMinWidth(0);
  selectionText->setHeight(36);
  selectionText->setAlign(TextView::CENTER);
  choiceRow->addView(selectionText);
  nextChoiceButton = makeButton(">", 48);
  nextChoiceButton->setOnClickListener([this]() { moveSelection(1); });
  choiceRow->addView(nextChoiceButton);
  addView(choiceRow);

  summaryText = makeText("", 15, ui_theme::textPrimary());
  summaryText->setHeight(26);
  summaryText->setAlign(TextView::CENTER);
  addView(summaryText);

  chartView = new AnalyticsChartView(
      model, [this](std::size_t first, std::size_t last) {
        selectSections(first, last);
      });
  chartView->setHeight(52);
  chartView->setMinHeight(44);
  chartView->setFlexGrow(1.0f);
  chartView->setFlexShrink(1.0f);
  chartView->setWidthPercent(100.0f);
  chartView->setCornerRadius(ui_theme::controlRadius());
  chartView->setBorderColor(ui_theme::hairlineSubtle());
  chartView->setBorderWidth(1);
  addView(chartView);

  detailText = makeText("", 13, ui_theme::textSecondary());
  detailText->setHeight(24);
  detailText->setAlign(TextView::CENTER);
  addView(detailText);
}

void PracticeAnalyticsView::setAttemptSelection(
    std::optional<std::size_t> attemptIndex) {
  model.selectAttempt(attemptIndex);
  if (attemptIndex.has_value()) {
    const std::size_t aggregateCount = model.compatibilityGroups().size();
    if (*attemptIndex < model.completedAttempts()) {
      choiceIndex = aggregateCount + *attemptIndex;
    }
  } else {
    choiceIndex = model.selectedAggregateGroup();
  }
  refreshText();
}

void PracticeAnalyticsView::setAggregateSelection(std::size_t groupIndex) {
  model.selectAggregateGroup(groupIndex);
  if (groupIndex < model.compatibilityGroups().size()) {
    choiceIndex = groupIndex;
  }
  refreshText();
}

void PracticeAnalyticsView::setMode(PracticeAnalyticsMode value) {
  mode = value;
  if (auto *chart = dynamic_cast<AnalyticsChartView *>(chartView)) {
    chart->setMode(value);
  }
  refreshText();
}

void PracticeAnalyticsView::setPhotoExportPresentation(
    bool showSharedInformation) {
  for (auto *button : modeButtons) {
    button->setDisplay(YGDisplayNone);
    button->setVisible(false);
  }
  if (previousChoiceButton != nullptr) {
    previousChoiceButton->setDisplay(YGDisplayNone);
    previousChoiceButton->setVisible(false);
  }
  if (nextChoiceButton != nullptr) {
    nextChoiceButton->setDisplay(YGDisplayNone);
    nextChoiceButton->setVisible(false);
  }
  if (modeControlsRow != nullptr) {
    modeControlsRow->setDisplay(YGDisplayNone);
    modeControlsRow->setVisible(false);
  }
  if (choiceRow != nullptr) {
    choiceRow->setDisplay(showSharedInformation ? YGDisplayFlex
                                                : YGDisplayNone);
    choiceRow->setVisible(showSharedInformation);
  }
  if (summaryText != nullptr) {
    summaryText->setDisplay(showSharedInformation ? YGDisplayFlex
                                                  : YGDisplayNone);
    summaryText->setVisible(showSharedInformation);
  }
}

void PracticeAnalyticsView::setSectionSelectionListener(
    std::function<void(long long, long long)> listener) {
  sectionSelectionListener = std::move(listener);
}

std::optional<practice::RangeSelection>
PracticeAnalyticsView::selectedSection() const {
  return model.selectedRange();
}

void PracticeAnalyticsView::moveSelection(int delta) {
  if (choices.empty()) {
    return;
  }
  const auto count = static_cast<long long>(choices.size());
  const auto current = static_cast<long long>(choiceIndex);
  choiceIndex = static_cast<std::size_t>((current + delta + count) % count);
  applyChoice();
}

void PracticeAnalyticsView::applyChoice() {
  if (choiceIndex >= choices.size()) {
    return;
  }
  const auto &choice = choices[choiceIndex];
  if (choice.attemptIndex.has_value()) {
    model.selectAttempt(choice.attemptIndex);
  } else {
    model.selectAggregateGroup(choice.groupIndex);
  }
  refreshText();
}

void PracticeAnalyticsView::refreshText() {
  if (selectionText == nullptr || summaryText == nullptr ||
      detailText == nullptr) {
    return;
  }
  if (model.selectedAttempt().has_value()) {
    selectionText->setText(model.attemptLabel(*model.selectedAttempt()));
  } else if (model.selectedAggregateGroup() <
             model.compatibilityGroups().size()) {
    selectionText->setText(
        "Aggregate · " +
        model.compatibilityGroups()[model.selectedAggregateGroup()].label);
  } else {
    selectionText->setText("No completed attempts");
  }

  const auto &analysis = model.displayedAnalysis();
  const auto &timing = analysis.overall;
  std::ostringstream summary;
  summary << (model.displayedIsAuto() ? "Auto timing" : "Timing")
          << " · Samples " << timing.samples << " · Misses " << timing.misses
          << " · Mean " << formatMetric(timing.meanMillis) << " · SD "
          << formatMetric(timing.standardDeviationMillis) << " · Median "
          << formatMetric(timing.medianMillis);
  summaryText->setText(summary.str());

  std::ostringstream detail;
  if (mode == PracticeAnalyticsMode::Histogram) {
    detail << "Early ← 0 → Late · 5 ms bins";
  } else if (mode == PracticeAnalyticsMode::Lanes) {
    detail << "Lane offsets";
    const std::size_t shown = std::min<std::size_t>(analysis.lanes.size(), 8);
    for (std::size_t index = 0; index < shown; ++index) {
      detail << (index == 0 ? " · " : "   ") << "L"
             << analysis.lanes[index].lane << ' '
             << formatMetric(analysis.lanes[index].timing.meanMillis);
    }
  } else if (const auto selected = model.selectedRange();
             selected.has_value()) {
    detail << "Selected " << std::fixed << std::setprecision(3)
           << static_cast<double>(selected->startMicros) / 1'000'000.0 << "–"
           << static_cast<double>(selected->endMicros) / 1'000'000.0
           << " s · exact measure boundaries";
  } else {
    detail << "Tap or drag measures to select an exact chart range";
  }
  if (model.displayedContainsAuto() && !model.displayedIsAuto()) {
    detail << " · aggregate includes Auto";
  }
  detailText->setText(modeName(mode) + " · " + detail.str());
}

void PracticeAnalyticsView::selectSections(std::size_t firstSection,
                                           std::size_t lastSection) {
  model.selectSection(firstSection, lastSection);
  refreshText();
  if (const auto selected = model.selectedRange();
      selected.has_value() && sectionSelectionListener) {
    sectionSelectionListener(selected->startMicros, selected->endMicros);
  }
}
