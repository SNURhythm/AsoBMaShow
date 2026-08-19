#pragma once

#include "BeatorajaHiSpeed.h"
#include "PlayfieldVisualState.h"
#include "../../skin/beatoraja/Skin2DRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace gameplay_visible_time {

// Mirrors LaneRenderer.currentduration: the live, cover-adjusted duration is
// derived from the same configured Hi-Speed that determines note travel.
[[nodiscard]] inline std::optional<int> currentDurationMilliseconds(
    double bpm, float configuredHispeed, int laneCoverPercent,
    bool laneCoverEnabled, double scrollRate = 1.0,
    double speedMultiplier = 1.0) {
  return gameplay_hispeed::liveDurationMilliseconds(
      bpm, configuredHispeed, laneCoverPercent, laneCoverEnabled, scrollRate,
      speedMultiplier);
}

[[nodiscard]] constexpr int durationToGreenNumber(int duration) noexcept {
  return gameplay_hispeed::durationToGreenNumber(duration);
}

} // namespace gameplay_visible_time

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
  // The raw LaneRenderer::getHispeed()-equivalent value exposed to skin
  // properties. `hispeed` remains the playback-scaled traversal speed.
  float configuredHispeed = 0.0F;
  float baseHispeed = 0.0F;
  float hispeed = 0.0F;
  float noteVisibleUpperBound = 0.0F;
  // Raw parser lane IDs in the exact white, blue, scratch traversal order.
  std::vector<int> playableLaneOrder;
  // BMSRendererState::currentTimelineIndex, expressed in the stable retained
  // model order. This is presentation state, not parser/gameplay state.
  std::uint32_t startRetainedOrdinal = 0;
};

// JsonPlaySkinObjectLoader applies one Note.dst2 value to every lane. The
// LaneRenderer missed-POOR path still reads lane zero's region and destination
// as its shared no-speed descent geometry.
struct PmsPoorDestinationGeometry {
  double laneOriginY = 0.0;
  double laneHeight = 0.0;
  double secondaryDestinationY = 0.0;
};

struct PlayfieldProjectionRequest {
  double visibleScrollBefore = 0.0;
  double visibleScrollAfter = 0.0;
  std::size_t maxTimelines = 0;
  std::size_t maxNotes = 0;
  bool includeInvisibleNotes = false;
  // PlayerConfig.showpastnote. LaneRenderer retains only an unprocessed
  // normal note after its timeline passes the judgement line.
  bool showPastNormalNotes = false;
  // PlayConfig.enableConstant and its two LaneRenderer time-window inputs.
  bool constantScroll = false;
  int constantDurationMilliseconds = 500;
  int constantFadeInMilliseconds = 100;
  // Practice count-in still renders notes at or after this immutable chart
  // boundary. A long-note pair that begins before it remains skipped.
  std::optional<long long> minimumVisibleNoteTimeMicros;
  // LaneRenderer emits BPM and STOP guide lines only with bpmguide enabled.
  bool bpmGuideEnabled = false;
  // The exact late-poor edge belongs to the active gameplay judge.  A caller
  // that only has an immutable visual snapshot must opt in explicitly rather
  // than projection guessing a ruleset-dependent value.
  std::int64_t latePoorTimingMicros = 0;
  // Present only for the selected skin's single JsonSkin.note/dst2 path.
  // The generic skin projection remains future-only when no source dst2 is
  // configured.
  std::optional<PmsPoorDestinationGeometry> pmsPoorDestination;
  // Selected skins consume generic DTOs only, so callers that do not submit
  // BMSRenderer's frame can omit its separate compatibility plan.
  bool buildBuiltInPlan = true;
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
  // LaneRenderer's dst2-only missed-POOR pass. The signed authored offset is
  // measured from the active Lift-adjusted judgement origin and must not be
  // multiplied by the regular Hi-Speed traversal.
  std::optional<double> pmsPoorYDisplacement;
  // LaneRenderer's Constant fade alpha. Non-Constant and opaque rows use 1.
  double opacity = 1.0;
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
  double opacity = 1.0;
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
  double opacity = 1.0;
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
  // This snapshot intentionally omits a BuiltInRendererPlan. BMSRenderer must
  // use its established parser-backed forward traversal instead of treating
  // the absent plan as a frame with no visible notes.
  bool useParserBackedBuiltInTraversal = false;
  // The built-in traversal is captured once by the gameplay renderer. Skin
  // property bridges consume its hispeed rather than reconstructing it from
  // settings or chart metadata.
  std::optional<BuiltInRendererTraversal> builtInTraversal;
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

#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
struct PlayfieldProjectionWorkStats {
  std::size_t timelineRowsExamined = 0;
  std::size_t noteDescriptorsExamined = 0;
  std::size_t longHeadsExamined = 0;
};
#endif

class PlayfieldProjection final {
public:
  [[nodiscard]] PlayfieldProjectionResult
  project(const PlayfieldChartVisualModel &, const PlayfieldVisualState &,
          const PlayfieldProjectionRequest &);
  void reset() noexcept;

#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
  [[nodiscard]] const PlayfieldProjectionWorkStats &
  lastWorkStatsForTesting() const noexcept {
    return lastWorkStats_;
  }
#endif

private:
  std::size_t pmsPoorCursor_ = 0;
  struct CachedModelIndex {
    const PlayfieldChartVisualModel *model = nullptr;
    std::unordered_map<ChartVisualId, const ChartVisualTimeline *>
        timelinesById;
    std::unordered_map<ChartVisualId, const ChartVisualTimeline *>
        previousTimelinesById;
    std::unordered_map<std::uint32_t, const ChartVisualTimeline *>
        retainedTimelinesByOrdinal;
    std::unordered_map<ChartVisualId, const ChartVisualNote *> notesById;
    std::unordered_map<ChartVisualId,
                       std::vector<const ChartVisualNote *>>
        notesByTimeline;
    std::vector<const ChartVisualTimeline *> orderedTimelines;
    std::vector<const ChartVisualTimeline *> retainedTimelines;
    std::vector<const ChartVisualNote *> orderedNotes;
    std::vector<gameplay_scroll_geometry::ScrollPositionTimeline>
        scrollTimelines;
  };

  void rebuildIndex(const PlayfieldChartVisualModel &);

  CachedModelIndex index_;
#if defined(ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING)
  PlayfieldProjectionWorkStats lastWorkStats_;
#endif
};

[[nodiscard]] double scrollPositionAtTime(const PlayfieldChartVisualModel &,
                                          long long timeMicros);
[[nodiscard]] PlayfieldSkinProjectionViews
adaptPlayfieldProjectionForSkin(const PlayfieldProjectionResult &);
