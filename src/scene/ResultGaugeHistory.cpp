#include "ResultGaugeHistory.h"

#include "../view/ClearLampColors.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace result_gauge_history {
namespace {

bool hasTypedHistory(const GameplayScoreState &state, GaugeType type) {
  return !state.gaugeHistoryFor(type).empty();
}

void appendUnique(std::vector<GaugeType> &types, GaugeType type) {
  if (std::ranges::find(types, type) == types.end()) {
    types.push_back(type);
  }
}

std::vector<GaugeType> availableTypes(const GameplayScoreState &state) {
  std::vector<GaugeType> types;
  types.reserve(kGaugeTypeCount);
  appendUnique(types, state.gaugeType);

  switch (state.gaugeAutoShift) {
  case GaugeAutoShiftMode::BestClear:
  case GaugeAutoShiftMode::SelectToUnder: {
    const int upper =
        state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder
            ? gaugeTypeIndex(state.selectedGaugeType)
            : (gaugeProfileIsCourse(state.gaugeProfile)
                   ? gaugeTypeIndex(GaugeType::ExHard)
                   : gaugeTypeIndex(GaugeType::Hazard));
    const int lower = gaugeProfileIsCourse(state.gaugeProfile)
                          ? gaugeTypeIndex(GaugeType::Normal)
                          : std::min(gaugeTypeIndex(
                                         state.gaugeAutoShiftLowerBound),
                                     upper);
    for (int index = upper; index >= lower; --index) {
      const GaugeType type = gaugeTypeAtIndex(index);
      if (hasTypedHistory(state, type)) {
        appendUnique(types, type);
      }
    }
    break;
  }
  case GaugeAutoShiftMode::SurvivalToGroove:
    if (hasTypedHistory(state, state.selectedGaugeType)) {
      appendUnique(types, state.selectedGaugeType);
    }
    if (hasTypedHistory(state, GaugeType::Normal)) {
      appendUnique(types, GaugeType::Normal);
    }
    break;
  case GaugeAutoShiftMode::None:
  case GaugeAutoShiftMode::Continue:
    break;
  }
  return types;
}

const std::vector<float> &historyFor(const GameplayScoreState &state,
                                     GaugeType type) {
  const auto &typed = state.gaugeHistoryFor(type);
  if (!typed.empty()) {
    return typed;
  }
  return state.gaugeHistory;
}

bool knownLampRank(int rank) {
  constexpr std::array ranks{
      kClearTypeFailedRank,
      kClearTypeAssistedEasyClearRank,
      kClearTypeLightAssistedEasyClearRank,
      kClearTypeEasyClearRank,
      kClearTypeNormalClearRank,
      kClearTypeHardClearRank,
      kClearTypeExHardClearRank,
      kClearTypeFullComboRank,
  };
  return std::ranges::find(ranks, rank) != ranks.end();
}

std::optional<int> rankForKnownGaugeLabel(std::string_view label) {
  if (label == "A-EASY" || label == "ASSISTED EASY" ||
      label == "ASSIST EASY") {
    return kClearTypeLightAssistedEasyClearRank;
  }
  if (label == "EASY") {
    return kClearTypeEasyClearRank;
  }
  if (label == "NORMAL") {
    return kClearTypeNormalClearRank;
  }
  if (label == "HARD") {
    return kClearTypeHardClearRank;
  }
  if (label == "EX-HARD") {
    return kClearTypeExHardClearRank;
  }
  if (label == "HAZARD") {
    return kClearTypeFullComboRank;
  }
  return std::nullopt;
}

std::optional<int> semanticRank(const ResultGaugeSeries &series) {
  if (series.clearRank.has_value() && knownLampRank(*series.clearRank)) {
    return series.clearRank;
  }
  if (series.label.has_value()) {
    return rankForKnownGaugeLabel(*series.label);
  }
  return std::nullopt;
}

std::optional<ResultGaugeGraphLabel>
labelFor(const ResultGaugeSeries &series) {
  const auto rank = semanticRank(series);
  if (series.label.has_value() && !series.label->empty()) {
    return ResultGaugeGraphLabel{
        .text = *series.label,
        .background = rank.has_value() ? clearLampColorForRank(*rank)
                                      : ui_theme::textMuted(),
    };
  }
  if (rank.has_value()) {
    return ResultGaugeGraphLabel{
        .text = clearTypeRankToLabel(*rank),
        .background = clearLampColorForRank(*rank),
    };
  }
  return std::nullopt;
}

Color lineColor(float value) {
  if (value > 80.0F) {
    return ui_theme::withAlpha(ui_theme::cyan(), 210);
  }
  if (value > 30.0F) {
    return ui_theme::withAlpha(ui_theme::lime(), 210);
  }
  return ui_theme::withAlpha(ui_theme::coral(), 210);
}

ResultGaugeGraphGeometry geometryFor(const ResultGaugeSeries &series) {
  ResultGaugeGraphGeometry geometry;
  const std::size_t count = series.points.size();
  const float maximum = series.maximum > 0.0F ? series.maximum : 100.0F;
  const auto normalizedY = [maximum](float value) {
    return 1.0F - std::clamp(value, 0.0F, maximum) / maximum;
  };
  geometry.guide80Y = normalizedY(80.0F);
  geometry.guide30Y = normalizedY(30.0F);

  std::vector<ResultGaugeGraphPoint> presentPoints;
  presentPoints.reserve(count);
  ResultGaugeGraphStrip *strip = nullptr;
  for (std::size_t index = 0; index < count; ++index) {
    if (!series.points[index].has_value()) {
      strip = nullptr;
      continue;
    }

    const float value = std::clamp(*series.points[index], 0.0F, maximum);
    ResultGaugeGraphPoint point{
        .index = index,
        .normalizedX = count <= 1
                           ? 0.0F
                           : static_cast<float>(index) /
                                 static_cast<float>(count - 1),
        .normalizedY = normalizedY(value),
        .value = value,
        .color = lineColor(value),
    };
    presentPoints.push_back(point);

    if (strip == nullptr) {
      geometry.strips.push_back({});
      strip = &geometry.strips.back();
    } else {
      geometry.segments.push_back({.from = strip->points.back(), .to = point});
    }
    strip->points.push_back(point);
  }

  const std::size_t markerStep =
      std::max<std::size_t>(1, presentPoints.size() / 40);
  for (std::size_t index = 0; index < presentPoints.size();
       index += markerStep) {
    geometry.markers.push_back(presentPoints[index]);
  }
  return geometry;
}

} // namespace

std::vector<ResultGaugeSeries> seriesFor(const GameplayScoreState &state) {
  std::vector<ResultGaugeSeries> result;
  for (const GaugeType type : availableTypes(state)) {
    const auto &history = historyFor(state, type);
    if (history.empty()) {
      continue;
    }

    ResultGaugeSeries series;
    series.points.reserve(history.size());
    for (const float value : history) {
      series.points.emplace_back(value);
    }
    series.label = gaugeDisplayShortLabel(type, state.gaugeProfile);
    series.clearRank = gaugeTypeToClearRank(type);
    series.maximum = gaugeMaximumValue(type, state.gaugeProfile);
    result.push_back(std::move(series));
  }
  return result;
}

std::size_t nextSeriesIndex(std::span<const ResultGaugeSeries> series,
                            std::size_t current) {
  if (series.empty() || current >= series.size() || current + 1 >= series.size()) {
    return 0;
  }
  return current + 1;
}

bool hasPresentPoints(const ResultGaugeSeries &series) noexcept {
  return std::ranges::any_of(series.points,
                             [](const auto &point) { return point.has_value(); });
}

std::optional<ResultGaugeGraph>
graphFor(std::span<const ResultGaugeSeries> series, std::size_t selectedIndex) {
  if (series.empty()) {
    return std::nullopt;
  }
  const std::size_t normalizedIndex =
      selectedIndex < series.size() ? selectedIndex : 0;
  const ResultGaugeSeries &selected = series[normalizedIndex];
  if (!hasPresentPoints(selected)) {
    return std::nullopt;
  }
  return ResultGaugeGraph{
      .seriesIndex = normalizedIndex,
      .geometry = geometryFor(selected),
      .label = labelFor(selected),
  };
}

} // namespace result_gauge_history
