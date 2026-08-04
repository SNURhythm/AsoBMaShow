#pragma once

#include "PlayfieldVisualState.h"
#include "../../skin/beatoraja/Skin2DRenderer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

enum class ProjectedLineKind : std::uint8_t {
  Section,
  BpmChange,
  Stop,
  Time,
};

struct PlayfieldProjectionRequest {
  double visibleScrollBefore = 0.0;
  double visibleScrollAfter = 0.0;
  std::size_t maxTimelines = 0;
  std::size_t maxNotes = 0;
  bool includeInvisibleNotes = false;
};

struct ProjectedTimelineDescriptor {
  ChartVisualId timelineId = 0;
  double scrollDelta = 0.0;
  long long timeMicros = 0;
  std::uint32_t submissionOrdinal = 0;
};

struct ProjectedPlayfieldNote {
  ChartVisualId noteId = 0;
  int lane = -1;
  ChartVisualNoteKind kind = ChartVisualNoteKind::Normal;
  double scrollDelta = 0.0;
  bool judged = false;
  std::uint32_t submissionOrdinal = 0;
};

struct ProjectedLongNoteDescriptor {
  ChartVisualId headId = 0;
  ChartVisualId tailId = 0;
  int lane = -1;
  ChartLongNoteMode mode = ChartLongNoteMode::LN;
  double headScrollDelta = 0.0;
  double tailScrollDelta = 0.0;
  bool active = false;
  bool damaged = false;
  bool reactive = false;
  bool headJudged = false;
  bool tailJudged = false;
  std::uint32_t submissionOrdinal = 0;
};

struct ProjectedLineDescriptor {
  ChartVisualId timelineId = 0;
  ProjectedLineKind kind = ProjectedLineKind::Time;
  double scrollDelta = 0.0;
  std::uint32_t submissionOrdinal = 0;
};

struct PlayfieldProjectionResult {
  std::uint64_t frameSerial = 0;
  double currentScrollPosition = 0.0;
  std::vector<ProjectedTimelineDescriptor> timelines;
  std::vector<ProjectedPlayfieldNote> notes;
  std::vector<ProjectedLongNoteDescriptor> longNotes;
  std::vector<ProjectedLineDescriptor> lines;
  bool budgetExceeded = false;
};

struct PlayfieldSkinProjectionViews {
  std::vector<skin::SkinProjectedNoteView> notes;
  std::vector<skin::SkinProjectedLongNoteView> longNotes;
  std::vector<skin::SkinProjectedLineView> lines;
};

class PlayfieldProjection final {
public:
  [[nodiscard]] PlayfieldProjectionResult
  project(const PlayfieldChartVisualModel &, const PlayfieldVisualState &,
          const PlayfieldProjectionRequest &);
  void reset() noexcept;
};

[[nodiscard]] double scrollPositionAtTime(const PlayfieldChartVisualModel &,
                                          long long timeMicros);
[[nodiscard]] PlayfieldSkinProjectionViews
adaptPlayfieldProjectionForSkin(const PlayfieldProjectionResult &);
