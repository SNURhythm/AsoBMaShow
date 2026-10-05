#include "ResultRecordSummary.h"
#include "DisplayTime.h"

#include "ReplayAutoPlay.h"
#include "ReplayClearMarkUtils.h"
#include "ResultContracts.h"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace {

bool validRemoteOriginIdentity(std::string_view providerId,
                               std::string_view serverOrigin) noexcept {
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  return ir::isValidProviderId(providerId) && normalizedOrigin &&
         *normalizedOrigin == serverOrigin;
}

bool validLinkedRemoteIdentity(const IrRemoteRecordId &identity) noexcept {
  return validRemoteOriginIdentity(identity.providerId,
                                   identity.serverOrigin) &&
         !identity.remoteScoreId.empty() &&
         identity.remoteScoreId.size() <= ir::kMaximumIrRemoteValueBytes &&
         std::ranges::none_of(identity.remoteScoreId,
                              [](unsigned char character) {
                                return character < 0x20U ||
                                       character == 0x7fU;
                              });
}

std::int64_t parseDisplayedTime(std::string_view value) noexcept {
  return display_time::parseUtcTimestamp(value).value_or(0);
}

std::string formatUnixMillis(std::int64_t unixMillis) {
  return unixMillis <= 0 ? std::string{} : display_time::formatUnixMillis(
      unixMillis, display_time::Precision::Milliseconds);
}

bool actionEnabled(const ResultRecordCapabilities &capabilities,
                   ResultRecordAction action) noexcept {
  switch (action) {
  case ResultRecordAction::Watch:
    return capabilities.watch;
  case ResultRecordAction::RetrySame:
    return capabilities.retrySame;
  case ResultRecordAction::GBattle:
    return capabilities.gBattle;
  case ResultRecordAction::PracticeGhost:
    return capabilities.practiceGhost;
  case ResultRecordAction::ResultRecall:
    return capabilities.resultRecall;
  case ResultRecordAction::VideoExport:
    return capabilities.videoExport;
  case ResultRecordAction::ShareOrCopy:
    return capabilities.shareOrCopy;
  case ResultRecordAction::DeleteReplayFile:
    return capabilities.deleteReplayFile;
  case ResultRecordAction::IrUpload:
    return capabilities.irUpload;
  }
  return false;
}

void appendFramed(std::string &key, std::string_view value) {
  key += std::to_string(value.size());
  key += ':';
  key.append(value);
}

void combineHash(std::size_t &seed, std::size_t value) noexcept {
  seed ^= value + static_cast<std::size_t>(0x9e3779b9U) + (seed << 6U) +
          (seed >> 2U);
}

ResultRecordCapabilities
projectedCapabilities(const replay::ReplayCapabilities &capabilities) noexcept {
  return {
      .watch = capabilities.watch,
      .retrySame = capabilities.retrySame,
      .gBattle = capabilities.gBattle,
      .practiceGhost = capabilities.practiceGhost,
      .resultRecall = capabilities.viewResult,
      .videoExport = capabilities.videoExport,
      .shareOrCopy = capabilities.shareOrCopy,
      .deleteReplayFile = capabilities.deleteReplayFile,
      .irUpload = capabilities.irUpload,
  };
}

} // namespace

std::size_t ResultRecordIdentityHash::operator()(
    const ResultRecordIdentity &identity) const noexcept {
  std::size_t seed = std::hash<std::size_t>{}(identity.index());
  std::visit(
      [&seed](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, AutoPlayRecordId>) {
          // The variant tag fully identifies the single synthetic row.
        } else if constexpr (std::is_same_v<Value, LegacyChartRecordId>) {
          combineHash(seed, std::hash<int>{}(value.legacyReplayId));
        } else if constexpr (std::is_same_v<Value, LegacyCourseRecordId>) {
          combineHash(seed, std::hash<int>{}(value.legacyCourseReplayId));
        } else if constexpr (std::is_same_v<Value, ModernChartRecordId> ||
                             std::is_same_v<Value, ModernCourseRecordId>) {
          combineHash(seed, std::hash<std::string>{}(value.attemptId));
        } else {
          combineHash(seed, std::hash<std::string>{}(value.providerId));
          combineHash(seed, std::hash<std::string>{}(value.serverOrigin));
          combineHash(seed, std::hash<std::string>{}(value.remoteScoreId));
        }
      },
      identity);
  return seed;
}

bool ResultRecordSummary::isLocal() const noexcept {
  return std::holds_alternative<AutoPlayRecordId>(identity) ||
         std::holds_alternative<ModernChartRecordId>(identity) ||
         std::holds_alternative<ModernCourseRecordId>(identity) ||
         isLegacyChart() || isLegacyCourse();
}

bool ResultRecordSummary::isModernChart() const noexcept {
  return std::holds_alternative<ModernChartRecordId>(identity);
}

bool ResultRecordSummary::isModernCourse() const noexcept {
  return std::holds_alternative<ModernCourseRecordId>(identity);
}

bool ResultRecordSummary::isLegacyChart() const noexcept {
  return std::holds_alternative<LegacyChartRecordId>(identity);
}

bool ResultRecordSummary::isLegacyCourse() const noexcept {
  return std::holds_alternative<LegacyCourseRecordId>(identity);
}

bool ResultRecordSummary::isRemote() const noexcept {
  return std::holds_alternative<IrRemoteRecordId>(identity);
}

std::optional<std::string_view>
ResultRecordSummary::modernAttemptId() const noexcept {
  if (const auto *modernIdentity =
          std::get_if<ModernChartRecordId>(&identity)) {
    return modernIdentity->attemptId;
  }
  if (const auto *modernIdentity =
          std::get_if<ModernCourseRecordId>(&identity)) {
    return modernIdentity->attemptId;
  }
  return std::nullopt;
}

std::optional<std::string_view>
ResultRecordSummary::remoteScoreId() const noexcept {
  const auto *remoteIdentity = std::get_if<IrRemoteRecordId>(&identity);
  return remoteIdentity
             ? std::optional<std::string_view>(remoteIdentity->remoteScoreId)
             : std::nullopt;
}

std::string ResultRecordSummary::stableKey() const {
  if (std::holds_alternative<AutoPlayRecordId>(identity)) {
    return "a:auto-play";
  }
  if (const auto *modernIdentity =
          std::get_if<ModernChartRecordId>(&identity)) {
    return "m:" + modernIdentity->attemptId;
  }
  if (const auto *modernIdentity =
          std::get_if<ModernCourseRecordId>(&identity)) {
    return "c:" + modernIdentity->attemptId;
  }
  if (const auto *legacyIdentity =
          std::get_if<LegacyChartRecordId>(&identity)) {
    return "lc:" + std::to_string(legacyIdentity->legacyReplayId);
  }
  if (const auto *legacyIdentity =
          std::get_if<LegacyCourseRecordId>(&identity)) {
    return "lco:" + std::to_string(legacyIdentity->legacyCourseReplayId);
  }

  const auto &remoteIdentity = std::get<IrRemoteRecordId>(identity);
  std::string key;
  key.reserve(remoteIdentity.providerId.size() +
              remoteIdentity.serverOrigin.size() +
              remoteIdentity.remoteScoreId.size() + 32);
  key = "r:";
  appendFramed(key, remoteIdentity.providerId);
  appendFramed(key, remoteIdentity.serverOrigin);
  appendFramed(key, remoteIdentity.remoteScoreId);
  return key;
}

ResultRecordActionTarget
resultRecordActionTarget(const ResultRecordSummary &summary,
                         ResultRecordAction action) noexcept {
  if (!actionEnabled(summary.capabilities, action) ||
      summary.legacyChart.has_value() || summary.legacyCourse.has_value()) {
    return ResultRecordActionTarget::None;
  }

  ResultRecordActionTarget target = ResultRecordActionTarget::None;
  if (const auto *identity =
          std::get_if<ModernChartRecordId>(&summary.identity);
      identity != nullptr && summary.modern.has_value() &&
      summary.modern->result.attemptId == identity->attemptId &&
      !summary.autoPlayReplay.has_value() &&
      !summary.modernCourse.has_value() &&
      !summary.remote.has_value() && !summary.autoPlay && !summary.course) {
    target = ResultRecordActionTarget::ModernChart;
  } else if (const auto *identity =
                 std::get_if<ModernCourseRecordId>(&summary.identity);
             identity != nullptr && summary.modernCourse.has_value() &&
             summary.modernCourse->result.attemptId == identity->attemptId &&
             !summary.autoPlayReplay.has_value() &&
             !summary.modern.has_value() &&
             !summary.remote.has_value() && !summary.autoPlay &&
             summary.course) {
    target = ResultRecordActionTarget::ModernCourse;
  } else if (const auto *identity =
                 std::get_if<IrRemoteRecordId>(&summary.identity);
             identity != nullptr && summary.remote.has_value() &&
             summary.remote->remoteScoreId == identity->remoteScoreId &&
             !summary.autoPlayReplay.has_value() &&
             !summary.modern.has_value() &&
             !summary.modernCourse.has_value() && !summary.autoPlay &&
             !summary.course) {
    target = ResultRecordActionTarget::Remote;
  } else if (std::holds_alternative<AutoPlayRecordId>(summary.identity) &&
             summary.autoPlayReplay.has_value() &&
             summary.autoPlayReplay->id == replay_autoplay::kReplayId &&
             summary.autoPlayReplay->autoPlay && summary.autoPlay &&
             !summary.course &&
             !summary.modern.has_value() && !summary.modernCourse.has_value() &&
             !summary.remote.has_value()) {
    target = ResultRecordActionTarget::AutoPlay;
  }

  switch (action) {
  case ResultRecordAction::Watch:
  case ResultRecordAction::VideoExport:
    return target == ResultRecordActionTarget::AutoPlay ||
                   target == ResultRecordActionTarget::ModernChart ||
                   target == ResultRecordActionTarget::ModernCourse
               ? target
               : ResultRecordActionTarget::None;
  case ResultRecordAction::RetrySame:
    return target == ResultRecordActionTarget::ModernChart ||
                   target == ResultRecordActionTarget::ModernCourse
               ? target
               : ResultRecordActionTarget::None;
  case ResultRecordAction::GBattle:
  case ResultRecordAction::PracticeGhost:
  case ResultRecordAction::IrUpload:
    return target == ResultRecordActionTarget::ModernChart
               ? target
               : ResultRecordActionTarget::None;
  case ResultRecordAction::ResultRecall:
    return target == ResultRecordActionTarget::ModernChart ||
                   target == ResultRecordActionTarget::ModernCourse ||
                   target == ResultRecordActionTarget::Remote
               ? target
               : ResultRecordActionTarget::None;
  case ResultRecordAction::ShareOrCopy:
  case ResultRecordAction::DeleteReplayFile:
    return target == ResultRecordActionTarget::ModernChart ||
                   target == ResultRecordActionTarget::ModernCourse
               ? target
               : ResultRecordActionTarget::None;
  }
  return ResultRecordActionTarget::None;
}

ResultRecordSummary makeAutoPlayResultRecord(ReplaySummary summary) {
  if (!summary.autoPlay || summary.courseReplay ||
      summary.id != replay_autoplay::kReplayId) {
    throw std::invalid_argument(
        "only the synthetic Auto Play summary may enter Records");
  }
  ResultRecordSummary result{
      .identity = AutoPlayRecordId{},
      .capabilities =
          {
              .watch = true,
              .videoExport = true,
          },
      .course = false,
      .autoPlay = true,
      .score = summary.finalScore,
      .maxScore = summary.maxScore,
      .maxCombo = summary.maxCombo,
      .finalGauge = summary.finalGauge,
      .clearRank = replay_clear_mark::effectiveClearRank(summary),
      .displayedTimeUnixMillis = parseDisplayedTime(summary.createdAt),
      .displayedTime = display_time::formatStoredTimestamp(summary.createdAt),
      .playOption = summary.playOption,
      .playOption2 = summary.playOption2,
      .irState = ir::IrRecordState::Hidden,
      .autoPlayReplay = std::move(summary),
      .modern = std::nullopt,
      .modernCourse = std::nullopt,
      .replayState = replay::ReplayState::NotApplicable,
      .remote = std::nullopt,
  };
  return result;
}

ResultRecordSummary
makeLegacyChartResultRecord(LegacyChartResultSummary summary) {
  if (summary.legacyReplayId <= 0) {
    throw std::invalid_argument("legacy chart summary ID is invalid");
  }
  const auto capabilities = replay::capabilitiesFor({
      .origin = replay::RecordOrigin::LegacyChartSummary,
      .replayState = replay::ReplayState::NotApplicable,
  });
  return {
      .identity = LegacyChartRecordId{.legacyReplayId = summary.legacyReplayId},
      .capabilities = projectedCapabilities(capabilities),
      .course = false,
      .autoPlay = false,
      .score = summary.finalScore.value_or(0),
      .maxScore = 0,
      .maxCombo = summary.maxCombo,
      .finalGauge = summary.finalGauge,
      .clearRank = summary.clearType.value_or(kClearTypeFailedRank),
      .scoreAvailable = summary.finalScore.has_value(),
      .maxScoreAvailable = false,
      .clearRankAvailable = summary.clearType.has_value(),
      .displayedTimeUnixMillis =
          summary.createdAt ? parseDisplayedTime(*summary.createdAt) : 0,
      .displayedTime = display_time::formatStoredTimestamp(summary.createdAt.value_or("")),
      .playOption = std::nullopt,
      .irState = ir::IrRecordState::Hidden,
      .autoPlayReplay = std::nullopt,
      .modern = std::nullopt,
      .modernCourse = std::nullopt,
      .replayState = replay::ReplayState::NotApplicable,
      .remote = std::nullopt,
      .legacyChart = std::move(summary),
      .legacyCourse = std::nullopt,
  };
}

ResultRecordSummary
makeLegacyCourseResultRecord(LegacyCourseResultSummary summary) {
  if (summary.legacyCourseReplayId <= 0) {
    throw std::invalid_argument("legacy course summary ID is invalid");
  }
  const auto capabilities = replay::capabilitiesFor({
      .origin = replay::RecordOrigin::LegacyCourseSummary,
      .replayState = replay::ReplayState::NotApplicable,
  });
  return {
      .identity = LegacyCourseRecordId{.legacyCourseReplayId =
                                           summary.legacyCourseReplayId},
      .capabilities = projectedCapabilities(capabilities),
      .course = true,
      .autoPlay = false,
      .score = summary.finalScore.value_or(0),
      .maxScore = 0,
      .maxCombo = summary.maxCombo,
      .finalGauge = summary.finalGauge,
      .completedCharts = summary.completedCharts,
      .totalCharts = summary.totalCharts,
      .clearRank = summary.clearType.value_or(kClearTypeFailedRank),
      .scoreAvailable = summary.finalScore.has_value(),
      .maxScoreAvailable = false,
      .clearRankAvailable = summary.clearType.has_value(),
      .displayedTimeUnixMillis =
          summary.createdAt ? parseDisplayedTime(*summary.createdAt) : 0,
      .displayedTime = display_time::formatStoredTimestamp(summary.createdAt.value_or("")),
      .playOption = std::nullopt,
      .irState = ir::IrRecordState::Hidden,
      .autoPlayReplay = std::nullopt,
      .modern = std::nullopt,
      .modernCourse = std::nullopt,
      .replayState = replay::ReplayState::NotApplicable,
      .remote = std::nullopt,
      .legacyChart = std::nullopt,
      .legacyCourse = std::move(summary),
  };
}

ResultRecordSummary
makeModernChartResultRecord(ModernChartResultRecord record,
                            replay::ReplayState replayState,
                            ir::IrRecordState irState,
                            std::optional<IrRemoteRecordId> linkedRemote) {
  if (record.result.resultId <= 0 || record.result.attemptId.empty() ||
      (linkedRemote && !validLinkedRemoteIdentity(*linkedRemote))) {
    throw std::invalid_argument("modern chart result is invalid");
  }
  if (isObsoleteRulesetDescriptor(record.result.score.provenance.ruleset)) {
    if (replayState == replay::ReplayState::Verified) {
      replayState = replay::ReplayState::Obsolete;
    }
    auto preservedRuleset = RulesetDescriptor::For(GameplayRuleset::LR2);
    preservedRuleset.version = 4;
    // The provider has already validated this IR state against the saved proof.
    // Rejudging its replay must not hide an eligible historical v4 score.
    if (record.result.score.provenance.ruleset != preservedRuleset) {
      irState = ir::IrRecordState::Hidden;
    }
  }
  const auto capabilities = replay::capabilitiesFor({
      .origin = replay::RecordOrigin::ModernChartResult,
      .replayState = replayState,
      .postponedIrSnapshotEligible = irState != ir::IrRecordState::Hidden,
  });
  const auto &result = record.result;
  ResultRecordSummary summary{
      .identity = ModernChartRecordId{.attemptId = result.attemptId},
      .capabilities = projectedCapabilities(capabilities),
      .course = false,
      .autoPlay = false,
      .score = result.score.score,
      .maxScore = result.score.maxScore,
      .maxCombo = result.score.maxCombo,
      .finalGauge = result.score.finalGauge,
      .clearRank = replay_clear_mark::effectiveClearRank(result.score),
      .displayedTimeUnixMillis = result.playedAtUnixMillis,
      .displayedTime = formatUnixMillis(result.playedAtUnixMillis),
      .playOption = result.score.provenance.player1.option,
      .playOption2 = result.score.provenance.player2.option,
      .irState = irState,
      .linkedRemote = std::move(linkedRemote),
      .autoPlayReplay = std::nullopt,
      .modern = std::move(record),
      .modernCourse = std::nullopt,
      .replayState = replayState,
      .remote = std::nullopt,
  };
  return summary;
}

ResultRecordSummary
makeModernCourseResultRecord(ModernCourseResultRecord record,
                             replay::ReplayState replayState) {
  if (record.result.resultId <= 0 || record.result.attemptId.empty() ||
      record.result.courseKey.empty()) {
    throw std::invalid_argument("modern course result is invalid");
  }
  if (replayState == replay::ReplayState::Verified &&
      isObsoleteRulesetDescriptor(record.result.provenance.ruleset)) {
    replayState = replay::ReplayState::Obsolete;
  }
  const auto capabilities = replay::capabilitiesFor({
      .origin = replay::RecordOrigin::ModernCourseResult,
      .replayState = replayState,
  });
  const auto &result = record.result;
  return {
      .identity = ModernCourseRecordId{.attemptId = result.attemptId},
      .capabilities = projectedCapabilities(capabilities),
      .course = true,
      .autoPlay = false,
      .score = result.finalScore,
      .maxScore = result.maxScore,
      .maxCombo = result.maxCombo,
      .finalGauge = result.finalGauge,
      .completedCharts = result.completedCharts,
      .totalCharts = result.totalCharts,
      .clearRank = result.clearType,
      .displayedTimeUnixMillis = result.playedAtUnixMillis,
      .displayedTime = formatUnixMillis(result.playedAtUnixMillis),
      .playOption = result.requestedPlayOption,
      .playOption2 = result.provenance.player2.option,
      .irState = ir::IrRecordState::Hidden,
      .autoPlayReplay = std::nullopt,
      .modern = std::nullopt,
      .modernCourse = std::move(record),
      .replayState = replayState,
      .remote = std::nullopt,
  };
}

ResultRecordSummary makeRemoteResultRecord(std::string_view providerId,
                                           std::string_view serverOrigin,
                                           ir::IrRemoteScore score) {
  if (!validRemoteOriginIdentity(providerId, serverOrigin)) {
    throw std::invalid_argument("IR remote record origin identity is invalid");
  }
  const std::optional<int> maximumScore =
      result_contract::maximumScoreForNotes(score.noteCount);
  std::string diagnostic;
  if (!maximumScore || !ir::validateIrRemoteScore(score, diagnostic)) {
    throw std::invalid_argument("IR remote record score is invalid");
  }

  const std::int64_t displayedTime =
      score.timeAchievedUnixMillis.value_or(score.timeAddedUnixMillis);
  const std::string remoteScoreId = score.remoteScoreId;
  ResultRecordSummary result{
      .identity =
          IrRemoteRecordId{
              .providerId = std::string(providerId),
              .serverOrigin = std::string(serverOrigin),
              .remoteScoreId = remoteScoreId,
          },
      .capabilities =
          {
              .watch = false,
              .gBattle = false,
              .resultRecall = true,
              .videoExport = false,
              .irUpload = false,
          },
      .course = false,
      .autoPlay = false,
      .score = score.score,
      .maxScore = *maximumScore,
      .maxCombo = score.maxCombo,
      .finalGauge = score.finalGauge,
      .clearRank = score.lampRank,
      .displayedTimeUnixMillis = displayedTime,
      .displayedTime = formatUnixMillis(displayedTime),
      .playOption = score.random,
      .irState = ir::IrRecordState::Uploaded,
      .autoPlayReplay = std::nullopt,
      .modern = std::nullopt,
      .modernCourse = std::nullopt,
      .replayState = replay::ReplayState::NotApplicable,
      .remote = std::move(score),
  };
  return result;
}

std::vector<ResultRecordSummary>
mergeResultRecords(std::span<const ReplaySummary> autoPlay,
                   std::span<const ir::IrRemoteScore> remote,
                   std::string_view providerId, std::string_view serverOrigin) {
  return mergeResultRecords(autoPlay, std::span<const ResultRecordSummary>{},
                            remote, providerId, serverOrigin);
}

std::vector<ResultRecordSummary>
mergeResultRecords(std::span<const ReplaySummary> autoPlay,
                   std::span<const ResultRecordSummary> projected,
                   std::span<const ir::IrRemoteScore> remote,
                   std::string_view providerId, std::string_view serverOrigin) {
  std::vector<ResultRecordSummary> result;
  std::unordered_set<std::string> linkedRemoteScoreIds;
  linkedRemoteScoreIds.reserve(projected.size());
  result.reserve(autoPlay.size() + projected.size() + remote.size());
  for (const ReplaySummary &summary : autoPlay) {
    result.push_back(makeAutoPlayResultRecord(summary));
  }
  for (const ResultRecordSummary &summary : projected) {
    const bool validChart = summary.isModernChart() &&
                            summary.modern.has_value() &&
                            !summary.modernCourse.has_value();
    const bool validCourse = summary.isModernCourse() &&
                             summary.modernCourse.has_value() &&
                             !summary.modern.has_value();
    const bool validLegacyChart =
        summary.isLegacyChart() && summary.legacyChart.has_value() &&
        !summary.legacyCourse.has_value() && !summary.modern.has_value() &&
        !summary.modernCourse.has_value();
    const bool validLegacyCourse =
        summary.isLegacyCourse() && summary.legacyCourse.has_value() &&
        !summary.legacyChart.has_value() && !summary.modern.has_value() &&
        !summary.modernCourse.has_value();
    if ((!validChart && !validCourse && !validLegacyChart &&
         !validLegacyCourse) ||
        summary.autoPlayReplay.has_value() || summary.remote.has_value() ||
        (summary.linkedRemote.has_value() &&
         (!validChart ||
          !validLinkedRemoteIdentity(*summary.linkedRemote)))) {
      throw std::invalid_argument("projected result record is invalid");
    }
    if (summary.linkedRemote &&
        summary.linkedRemote->providerId == providerId &&
        summary.linkedRemote->serverOrigin == serverOrigin) {
      linkedRemoteScoreIds.emplace(summary.linkedRemote->remoteScoreId);
    }
    result.push_back(summary);
  }
  for (const ir::IrRemoteScore &score : remote) {
    auto remoteSummary =
        makeRemoteResultRecord(providerId, serverOrigin, score);
    if (!linkedRemoteScoreIds.contains(score.remoteScoreId)) {
      result.push_back(std::move(remoteSummary));
    }
  }

  std::sort(
      result.begin(), result.end(),
      [](const ResultRecordSummary &left, const ResultRecordSummary &right) {
        if (left.autoPlay != right.autoPlay) {
          return left.autoPlay;
        }
        if (left.displayedTimeUnixMillis != right.displayedTimeUnixMillis) {
          return left.displayedTimeUnixMillis > right.displayedTimeUnixMillis;
        }
        return left.stableKey() < right.stableKey();
      });
  return result;
}
