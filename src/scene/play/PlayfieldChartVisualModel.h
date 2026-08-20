#pragma once

#include "GameplayScrollGeometry.h"

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
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
  // Pinned TimeLine#getSpeed() is the propagated multiplier. Only authored
  // SPEED points carry the matching hasSpeedObj marker.
  double speed = 1.0;
  bool hasSpeedObject = false;
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

struct ChartVisualSpeedPoint {
  long long timeMicros = 0;
  double speed = 1.0;

  bool operator==(const ChartVisualSpeedPoint &) const = default;
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
  std::string fullArtist;
  std::string genre;
  std::map<int, std::string> auditedStringProperties;

  bool operator==(const PlayfieldChartTextMetadata &) const = default;
};

// Immutable equivalent of Beatoraja SongData.getInformation(). A missing value
// remains observable because the source factories return their numeric
// sentinels when SongInformation has not been prepared.
struct PlayfieldSongInformation {
  double density = 0.0;
  double peakDensity = 0.0;
  double endDensity = 0.0;
  double total = 0.0;

  bool operator==(const PlayfieldSongInformation &) const = default;
};

// Immutable chart data used by static Beatoraja skin properties. This remains
// parser-free after conversion so projection and skin evaluation never need to
// retain the mutable parser chart.
struct PlayfieldChartStaticMetadata {
  int difficulty = 0;
  int judgeRank = 3;
  double minimumBpm = 0.0;
  double maximumBpm = 0.0;
  double mainBpm = 0.0;
  long long durationMicros = 0;
  std::string authoredPlayLevel;
  int playLevel = 0;
  int normalKeyNotes = 0;
  int longKeyNotes = 0;
  int normalScratchNotes = 0;
  int longScratchNotes = 0;
  int totalNotes = 0;
  int totalLandmineNotes = 0;
  // SongData's four long-note feature bits are consumed by
  // IntegerPropertyFactory.IndexType.lnmode.  Preserve their pre-override
  // parser meaning separately from the effective render mode.
  bool hasAnyLongNote = false;
  bool hasUndefinedLongNote = false;
  bool hasLongNote = false;
  bool hasChargeNote = false;
  bool hasHellChargeNote = false;
  int selectedLongNoteMode = 1;
  bool hasBga = false;
  bool hasRandomSequence = false;
  bool hasBpmStop = false;
  std::string stageFilePath;
  std::string backBmpPath;
  std::optional<PlayfieldSongInformation> songInformation;

  bool operator==(const PlayfieldChartStaticMetadata &) const = default;
};

struct ChartVisualBgaPoorSequence {
  long long startBgaMicros = 0;
  std::uint32_t authoredOrdinal = 0;
  std::vector<int> frames;

  bool operator==(const ChartVisualBgaPoorSequence &) const = default;
};

struct PlayfieldChartVisualModel {
  std::string chartMd5;
  std::string chartSha256;
  int keyCount = 0;
  PlayfieldChartTextMetadata text;
  PlayfieldChartStaticMetadata staticMetadata;
  std::vector<int> laneOrder;
  std::vector<ChartVisualTimeline> timelines;
  // Sparse authored SPEED points used by per-frame LaneRenderer-compatible
  // interpolation. Ordinary chart timelines never participate in lookup.
  std::vector<ChartVisualSpeedPoint> speedPoints;
  std::vector<ChartVisualNote> notes;
  std::vector<double> scrollPrefix;
  // The parser has no object for the end of a measure. This is a geometry-only
  // anchor, never a skin-visible chart timeline.
  std::optional<gameplay_scroll_geometry::ScrollPositionTimeline>
      terminalScrollAnchor;
  std::vector<ChartVisualBgaPoorSequence> bgaPoorSequences;

  [[nodiscard]] std::vector<std::string> runtimeStrings() const;

  bool operator==(const PlayfieldChartVisualModel &) const = default;
};

[[nodiscard]] PlayfieldChartVisualModel
buildPlayfieldChartVisualModel(const bms_parser::Chart &chart,
                               int longNoteModeOverride);

// LaneRenderer#getCurrentSpeed(), expressed against the immutable chart DTO.
[[nodiscard]] double
speedObjectMultiplierAtTime(const PlayfieldChartVisualModel &model,
                            long long timeMicros) noexcept;
