#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace bms_parser {
class Chart;
}

using ChartVisualId = std::uint32_t;
inline constexpr std::uint32_t kNoRetainedTimelineOrdinal =
    std::numeric_limits<std::uint32_t>::max();

enum class ChartVisualNoteKind : std::uint8_t {
  Normal,
  Invisible,
  Mine,
  LongHead,
  LongBody,
  LongTail,
};

enum class ChartVisualNoteSource : std::uint8_t {
  Playable,
  Invisible,
  Mine,
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
  // Index in Beatoraja-compatible retained render traversal. BGA-only and
  // other discarded rows intentionally use the explicit no-ordinal sentinel.
  std::uint32_t retainedOrdinal = kNoRetainedTimelineOrdinal;

  bool operator==(const ChartVisualTimeline &) const = default;
};

struct ChartVisualNote {
  ChartVisualId id = 0;
  ChartVisualId timelineId = 0;
  ChartVisualId pairId = 0;
  int lane = -1;
  ChartVisualNoteKind kind = ChartVisualNoteKind::Normal;
  // Channel origin stays independent from LongHead/LongTail endpoint kind.
  ChartVisualNoteSource source = ChartVisualNoteSource::Playable;
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

struct ChartVisualBgaPoorSequence {
  long long startBgaMicros = 0;
  std::uint32_t authoredOrdinal = 0;
  std::vector<int> frames;

  bool operator==(const ChartVisualBgaPoorSequence &) const = default;
};

struct PlayfieldChartVisualModel {
  std::string chartSha256;
  int keyCount = 0;
  PlayfieldChartTextMetadata text;
  std::vector<int> laneOrder;
  std::vector<ChartVisualTimeline> timelines;
  std::vector<ChartVisualNote> notes;
  std::vector<double> scrollPrefix;
  std::vector<ChartVisualBgaPoorSequence> bgaPoorSequences;

  [[nodiscard]] std::vector<std::string> runtimeStrings() const;

  bool operator==(const PlayfieldChartVisualModel &) const = default;
};

[[nodiscard]] PlayfieldChartVisualModel
buildPlayfieldChartVisualModel(const bms_parser::Chart &chart,
                               int longNoteModeOverride);
