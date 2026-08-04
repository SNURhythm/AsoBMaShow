#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bms_parser {
class Chart;
}

using ChartVisualId = std::uint32_t;

enum class ChartVisualNoteKind : std::uint8_t {
  Normal,
  Invisible,
  Mine,
  LongHead,
  LongBody,
  LongTail,
};

enum class ChartLongNoteMode : std::uint8_t { LN, CN, HCN };

struct ChartVisualTimeline {
  ChartVisualId id = 0;
  long long timeMicros = 0;
  double beat = 0.0;
  double scrollPosition = 0.0;
  double bpm = 0.0;
  double scrollRate = 1.0;
  // bms-parser-cpp does not expose SPEED/hasSpeedObj. Projection therefore
  // intentionally uses a fixed neutral speed until parser support exists.
  double speed = 1.0;
  long long stopMicros = 0;
  bool sectionLine = false;
  bool bgaOnly = false;
  bool retainedForProjection = false;
  std::uint32_t authoredOrdinal = 0;

  bool operator==(const ChartVisualTimeline &) const = default;
};

struct ChartVisualNote {
  ChartVisualId id = 0;
  ChartVisualId timelineId = 0;
  ChartVisualId pairId = 0;
  int lane = -1;
  ChartVisualNoteKind kind = ChartVisualNoteKind::Normal;
  ChartLongNoteMode longNoteMode = ChartLongNoteMode::LN;
  int mineDamage = 0;
  std::uint32_t authoredOrdinal = 0;

  bool operator==(const ChartVisualNote &) const = default;
};

struct PlayfieldChartTextMetadata {
  std::string title;
  std::string subtitle;
  std::string artist;
  std::string subartist;
  std::string genre;
  std::map<int, std::string> auditedStringProperties;

  bool operator==(const PlayfieldChartTextMetadata &) const = default;
};

struct PlayfieldChartVisualModel {
  std::string chartSha256;
  int keyCount = 0;
  PlayfieldChartTextMetadata text;
  std::vector<int> laneOrder;
  std::vector<ChartVisualTimeline> timelines;
  std::vector<ChartVisualNote> notes;
  std::vector<double> scrollPrefix;

  [[nodiscard]] std::vector<std::string> runtimeStrings() const;

  bool operator==(const PlayfieldChartVisualModel &) const = default;
};

[[nodiscard]] PlayfieldChartVisualModel
buildPlayfieldChartVisualModel(const bms_parser::Chart &chart,
                               int longNoteModeOverride);
