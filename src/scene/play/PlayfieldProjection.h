#pragma once

#include "PlayfieldVisualState.h"
#include "../../skin/beatoraja/Skin2DRenderer.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

enum class ProjectedLineKind : std::uint8_t {
  Section,
  BpmChange,
  Stop,
  Time,
};

// The built-in renderer follows Beatoraja's incremental forward walk rather
// than the generic skin viewport filter.  These values make that walk a
// value-only projection concern: no parser object crosses the frame boundary.
struct BuiltInRendererTraversal {
  float lowerBound = -1.0F;
  float judgeY = 0.0F;
  float upperBound = 0.0F;
  float rxhs = 0.0F;
  float hispeed = 0.0F;
  float noteVisibleUpperBound = 0.0F;
  // Raw parser lane IDs in the exact white, blue, scratch traversal order.
  std::vector<int> playableLaneOrder;
  // BMSRendererState::currentTimelineIndex, expressed in the stable retained
  // model order. This is presentation state, not parser/gameplay state.
  std::uint32_t startRetainedOrdinal = 0;
};

struct PlayfieldProjectionRequest {
  double visibleScrollBefore = 0.0;
  double visibleScrollAfter = 0.0;
  std::size_t maxTimelines = 0;
  std::size_t maxNotes = 0;
  bool includeInvisibleNotes = false;
  // The exact late-poor edge belongs to the active gameplay judge.  A caller
  // that only has an immutable visual snapshot must opt in explicitly rather
  // than projection guessing a ruleset-dependent value.
  std::int64_t latePoorTimingMicros = 0;
  std::optional<BuiltInRendererTraversal> builtInTraversal;
};

struct ProjectedTimelineDescriptor {
  ChartVisualId timelineId = 0;
  double scrollDelta = 0.0;
  long long timeMicros = 0;
  std::uint32_t authoredOrdinal = 0;
  std::uint32_t retainedOrdinal = kNoRetainedTimelineOrdinal;
  std::uint32_t submissionOrdinal = 0;
};

struct ProjectedPlayfieldNote {
  ChartVisualId noteId = 0;
  ChartVisualId timelineId = 0;
  ChartVisualId pairId = 0;
  int lane = -1;
  ChartVisualNoteKind kind = ChartVisualNoteKind::Normal;
  ChartVisualNoteSource source = ChartVisualNoteSource::Playable;
  ChartLongNoteMode longNoteMode = ChartLongNoteMode::LN;
  int mineDamage = 0;
  double scrollDelta = 0.0;
  long long timeMicros = 0;
  std::uint32_t authoredOrdinal = 0;
  std::uint32_t retainedTimelineOrdinal = kNoRetainedTimelineOrdinal;
  bool judged = false;
  std::uint32_t submissionOrdinal = 0;
  // The built-in renderer shares one primary depth across playable notes and
  // mines in a timeline row. Skin submission order stays unique and separate.
  std::uint32_t builtInDepth = 0;
};

struct ProjectedLongNoteDescriptor {
  ChartVisualId headId = 0;
  ChartVisualId tailId = 0;
  ChartVisualId headTimelineId = 0;
  ChartVisualId tailTimelineId = 0;
  int lane = -1;
  ChartLongNoteMode mode = ChartLongNoteMode::LN;
  ChartVisualNoteSource headSource = ChartVisualNoteSource::Playable;
  ChartVisualNoteSource tailSource = ChartVisualNoteSource::Playable;
  double headScrollDelta = 0.0;
  double tailScrollDelta = 0.0;
  long long headTimeMicros = 0;
  long long tailTimeMicros = 0;
  std::uint32_t headAuthoredOrdinal = 0;
  std::uint32_t tailAuthoredOrdinal = 0;
  std::uint32_t headRetainedOrdinal = kNoRetainedTimelineOrdinal;
  std::uint32_t tailRetainedOrdinal = kNoRetainedTimelineOrdinal;
  bool active = false;
  bool damaged = false;
  bool reactive = false;
  bool headPlayed = false;
  bool tailPlayed = false;
  bool headJudged = false;
  bool tailJudged = false;
  bool headDead = false;
  bool tailDead = false;
  long long headPlayedTimeMicros = kPlayfieldTimestampOff;
  long long tailPlayedTimeMicros = kPlayfieldTimestampOff;
  bool tailReleasedEarly = false;
  bool tailMissedWithHead = false;
  bool tailResolvedAtOrAfterTiming = false;
  std::uint32_t submissionOrdinal = 0;
  std::uint32_t bodyDepth = 0;
  std::uint32_t endpointDepth = 0;
};

struct ProjectedLineDescriptor {
  ChartVisualId timelineId = 0;
  ProjectedLineKind kind = ProjectedLineKind::Time;
  double scrollDelta = 0.0;
  long long timeMicros = 0;
  std::uint32_t authoredOrdinal = 0;
  std::uint32_t retainedOrdinal = kNoRetainedTimelineOrdinal;
  std::uint32_t submissionOrdinal = 0;
};

enum class BuiltInRendererPlanEntryKind : std::uint8_t {
  SectionLine,
  Note,
  LongNote,
};

// `descriptorIndex` addresses the matching owned vector in
// BuiltInRendererPlan. Long-note entries carry the exact result of legacy
// lookahead: a tail outside the bounded walk is rendered at upperBound.
struct BuiltInRendererPlanEntry {
  BuiltInRendererPlanEntryKind kind = BuiltInRendererPlanEntryKind::Note;
  std::uint32_t descriptorIndex = 0;
  bool tailAtUpperBound = false;
  float renderY = std::numeric_limits<float>::quiet_NaN();
  float tailRenderY = std::numeric_limits<float>::quiet_NaN();
};

struct BuiltInRendererPlanTimeline {
  std::uint32_t retainedOrdinal = kNoRetainedTimelineOrdinal;
  long long timeMicros = 0;
  float renderY = std::numeric_limits<float>::quiet_NaN();
  bool future = false;
};

struct BuiltInRendererPlan {
  std::vector<std::uint32_t> traversedTimelineOrdinals;
  std::vector<BuiltInRendererPlanTimeline> timelines;
  std::uint32_t nextStartRetainedOrdinal = 0;
  std::vector<ProjectedPlayfieldNote> notes;
  std::vector<ProjectedLongNoteDescriptor> longNotes;
  std::vector<ProjectedLineDescriptor> lines;
  std::vector<BuiltInRendererPlanEntry> entries;
};

struct PlayfieldProjectionResult {
  std::uint64_t frameSerial = 0;
  double currentScrollPosition = 0.0;
  std::vector<ProjectedTimelineDescriptor> timelines;
  std::vector<ProjectedPlayfieldNote> notes;
  std::vector<ProjectedLongNoteDescriptor> longNotes;
  std::vector<ProjectedLineDescriptor> lines;
  // Deliberately separate from the bounded generic skin DTOs above. This is
  // the immutable execution plan for BMSRenderer's compatibility path.
  BuiltInRendererPlan builtInPlan;
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
