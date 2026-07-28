#include "ResultRecordSummary.h"

#include "ReplayAutoPlay.h"
#include "ReplayClearMarkUtils.h"
#include "ResultContracts.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

bool validRemoteOriginIdentity(std::string_view providerId,
                               std::string_view serverOrigin) noexcept {
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  return ir::isValidProviderId(providerId) && normalizedOrigin &&
         *normalizedOrigin == serverOrigin;
}

bool isLeapYear(int year) noexcept {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int daysInMonth(int year, int month) noexcept {
  constexpr std::array days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > static_cast<int>(days.size())) {
    return 0;
  }
  return month == 2 && isLeapYear(year) ? 29 : days[month - 1];
}

std::optional<int> parseDigits(std::string_view text, std::size_t offset,
                               std::size_t count) noexcept {
  if (offset > text.size() || count > text.size() - offset) {
    return std::nullopt;
  }
  int value = 0;
  const char *begin = text.data() + offset;
  const char *end = begin + count;
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

// Howard Hinnant's civil-calendar transform, shifted to the Unix epoch.
std::int64_t daysFromCivil(int year, unsigned month, unsigned day) noexcept {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear =
      (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(dayOfEra) - 719468;
}

std::int64_t parseDisplayedTime(std::string_view value) noexcept {
  if (value.size() != 19 && value.size() != 23) {
    return 0;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != ' ' ||
      value[13] != ':' || value[16] != ':' ||
      (value.size() == 23 && value[19] != '.')) {
    return 0;
  }
  const auto year = parseDigits(value, 0, 4);
  const auto month = parseDigits(value, 5, 2);
  const auto day = parseDigits(value, 8, 2);
  const auto hour = parseDigits(value, 11, 2);
  const auto minute = parseDigits(value, 14, 2);
  const auto second = parseDigits(value, 17, 2);
  const auto millis =
      value.size() == 23 ? parseDigits(value, 20, 3) : std::optional<int>(0);
  if (!year || !month || !day || !hour || !minute || !second || !millis ||
      *day < 1 || *day > daysInMonth(*year, *month) || *hour > 23 ||
      *minute > 59 || *second > 59) {
    return 0;
  }
  const std::int64_t days = daysFromCivil(*year, static_cast<unsigned>(*month),
                                          static_cast<unsigned>(*day));
  return (((days * 24 + *hour) * 60 + *minute) * 60 + *second) * 1'000 +
         *millis;
}

std::string formatUnixMillis(std::int64_t unixMillis) {
  if (unixMillis <= 0) {
    return {};
  }
  const std::time_t seconds = static_cast<std::time_t>(unixMillis / 1'000);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &seconds) != 0) {
    return {};
  }
#else
  if (gmtime_r(&seconds, &utc) == nullptr) {
    return {};
  }
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3)
         << std::setfill('0') << (unixMillis % 1'000);
  return output.str();
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
      .clearRank = replay_clear_mark::effectiveClearRank(summary),
      .displayedTimeUnixMillis = parseDisplayedTime(summary.createdAt),
      .displayedTime = summary.createdAt,
      .playOption = summary.playOption,
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
      .clearRank = summary.clearType.value_or(kClearTypeFailedRank),
      .scoreAvailable = summary.finalScore.has_value(),
      .maxScoreAvailable = false,
      .clearRankAvailable = summary.clearType.has_value(),
      .displayedTimeUnixMillis =
          summary.createdAt ? parseDisplayedTime(*summary.createdAt) : 0,
      .displayedTime = summary.createdAt.value_or(""),
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
      .clearRank = summary.clearType.value_or(kClearTypeFailedRank),
      .scoreAvailable = summary.finalScore.has_value(),
      .maxScoreAvailable = false,
      .clearRankAvailable = summary.clearType.has_value(),
      .displayedTimeUnixMillis =
          summary.createdAt ? parseDisplayedTime(*summary.createdAt) : 0,
      .displayedTime = summary.createdAt.value_or(""),
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
                            ir::IrRecordState irState) {
  if (record.result.resultId <= 0 || record.result.attemptId.empty()) {
    throw std::invalid_argument("modern chart result is invalid");
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
      .clearRank = result.score.clearType,
      .displayedTimeUnixMillis = result.playedAtUnixMillis,
      .displayedTime = formatUnixMillis(result.playedAtUnixMillis),
      .playOption = result.score.provenance.player1.option,
      .irState = irState,
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
      .clearRank = result.clearType,
      .displayedTimeUnixMillis = result.playedAtUnixMillis,
      .displayedTime = formatUnixMillis(result.playedAtUnixMillis),
      .playOption = result.requestedPlayOption,
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
        summary.autoPlayReplay.has_value() || summary.remote.has_value()) {
      throw std::invalid_argument("projected result record is invalid");
    }
    result.push_back(summary);
  }
  for (const ir::IrRemoteScore &score : remote) {
    result.push_back(makeRemoteResultRecord(providerId, serverOrigin, score));
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
