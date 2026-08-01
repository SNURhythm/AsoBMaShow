#pragma once

#include "../rendering/Color.h"
#include "play/GameplayScoreState.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct ResultGaugeSeries {
  std::vector<std::optional<float>> points;
  std::optional<std::string> label;
  std::optional<int> clearRank;
  float maximum = 100.0F;

  bool operator==(const ResultGaugeSeries &) const = default;
};

namespace result_gauge_history {

struct ResultGaugeGraphPoint {
  std::size_t index = 0;
  float normalizedX = 0.0F;
  float normalizedY = 0.0F;
  float value = 0.0F;
  Color color;
};

struct ResultGaugeGraphStrip {
  std::vector<ResultGaugeGraphPoint> points;
};

struct ResultGaugeGraphSegment {
  ResultGaugeGraphPoint from;
  ResultGaugeGraphPoint to;
};

struct ResultGaugeGraphGeometry {
  std::vector<ResultGaugeGraphStrip> strips;
  std::vector<ResultGaugeGraphSegment> segments;
  std::vector<ResultGaugeGraphPoint> markers;
  float guide80Y = 0.2F;
  float guide30Y = 0.7F;
};

struct ResultGaugeGraphLabel {
  std::string text;
  Color background;
};

struct ResultGaugeGraph {
  std::size_t seriesIndex = 0;
  ResultGaugeGraphGeometry geometry;
  std::optional<ResultGaugeGraphLabel> label;
};

[[nodiscard]] std::vector<ResultGaugeSeries>
seriesFor(const GameplayScoreState &state);

[[nodiscard]] std::size_t
nextSeriesIndex(std::span<const ResultGaugeSeries> series,
                std::size_t current);

[[nodiscard]] bool hasPresentPoints(const ResultGaugeSeries &series) noexcept;

[[nodiscard]] std::optional<ResultGaugeGraph>
graphFor(std::span<const ResultGaugeSeries> series, std::size_t selectedIndex);

} // namespace result_gauge_history
