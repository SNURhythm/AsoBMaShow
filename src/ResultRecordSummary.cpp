#include "ResultRecordSummary.h"

#include "ReplayClearMarkUtils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace {

bool validProviderId(std::string_view value) noexcept {
  if (value.empty() || value.size() > ir::kMaximumIrProviderIdBytes ||
      value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-';
  });
}

bool validRemoteOriginIdentity(std::string_view providerId,
                               std::string_view serverOrigin) noexcept {
  const auto normalizedOrigin = ir::normalizeServerOrigin(serverOrigin);
  return validProviderId(providerId) && normalizedOrigin &&
         *normalizedOrigin == serverOrigin;
}

std::optional<int> checkedMaximumScore(int noteCount) noexcept {
  if (noteCount < 0 ||
      noteCount > std::numeric_limits<int>::max() / 2) {
    return std::nullopt;
  }
  return noteCount * 2;
}

bool isLeapYear(int year) noexcept {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int daysInMonth(int year, int month) noexcept {
  constexpr std::array days{31, 28, 31, 30, 31, 30,
                            31, 31, 30, 31, 30, 31};
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
  const std::int64_t days = daysFromCivil(
      *year, static_cast<unsigned>(*month), static_cast<unsigned>(*day));
  return (((days * 24 + *hour) * 60 + *minute) * 60 + *second) * 1'000 +
         *millis;
}

std::string formatUnixMillis(std::int64_t unixMillis) {
  if (unixMillis <= 0) {
    return {};
  }
  const std::time_t seconds =
      static_cast<std::time_t>(unixMillis / 1'000);
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
  output << std::put_time(&utc, "%Y-%m-%d %H:%M:%S") << '.'
         << std::setw(3) << std::setfill('0') << (unixMillis % 1'000);
  return output.str();
}

bool irUploadActionable(ir::IrRecordState state) noexcept {
  return state == ir::IrRecordState::Eligible ||
         state == ir::IrRecordState::Failed;
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

} // namespace

std::size_t ResultRecordIdentityHash::operator()(
    const ResultRecordIdentity &identity) const noexcept {
  std::size_t seed = std::hash<std::size_t>{}(identity.index());
  std::visit(
      [&seed](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, LocalReplayRecordId>) {
          combineHash(seed, std::hash<int>{}(value.replayId));
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
  return std::holds_alternative<LocalReplayRecordId>(identity);
}

bool ResultRecordSummary::isRemote() const noexcept {
  return std::holds_alternative<IrRemoteRecordId>(identity);
}

std::optional<int> ResultRecordSummary::localReplayId() const noexcept {
  const auto *localIdentity = std::get_if<LocalReplayRecordId>(&identity);
  return localIdentity ? std::optional<int>(localIdentity->replayId)
                       : std::nullopt;
}

std::optional<std::string_view>
ResultRecordSummary::remoteScoreId() const noexcept {
  const auto *remoteIdentity = std::get_if<IrRemoteRecordId>(&identity);
  return remoteIdentity
             ? std::optional<std::string_view>(remoteIdentity->remoteScoreId)
             : std::nullopt;
}

std::string ResultRecordSummary::stableKey() const {
  if (const auto *localIdentity =
          std::get_if<LocalReplayRecordId>(&identity)) {
    return "l:" + std::to_string(localIdentity->replayId);
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

ResultRecordSummary makeLocalResultRecord(ReplaySummary summary) {
  ResultRecordSummary result{
      .identity = LocalReplayRecordId{.replayId = summary.id},
      .capabilities =
          {
              .watch = true,
              .gBattle = !summary.autoPlay && !summary.courseReplay,
              .resultRecall = !summary.autoPlay,
              .videoExport = true,
              .irUpload = irUploadActionable(summary.irRecordState),
          },
      .course = summary.courseReplay,
      .autoPlay = summary.autoPlay,
      .score = summary.finalScore,
      .maxScore = summary.maxScore,
      .maxCombo = summary.maxCombo,
      .clearRank = replay_clear_mark::effectiveClearRank(summary),
      .displayedTimeUnixMillis = parseDisplayedTime(summary.createdAt),
      .displayedTime = summary.createdAt,
      .playOption = summary.playOption,
      .irState = summary.irRecordState,
      .local = std::move(summary),
      .remote = std::nullopt,
  };
  return result;
}

ResultRecordSummary makeRemoteResultRecord(std::string_view providerId,
                                           std::string_view serverOrigin,
                                           ir::IrRemoteScore score) {
  if (!validRemoteOriginIdentity(providerId, serverOrigin)) {
    throw std::invalid_argument("IR remote record origin identity is invalid");
  }
  const std::optional<int> maximumScore = checkedMaximumScore(score.noteCount);
  std::string diagnostic;
  if (!maximumScore || !ir::validateIrRemoteScore(score, diagnostic)) {
    throw std::invalid_argument("IR remote record score is invalid");
  }

  const std::int64_t displayedTime =
      score.timeAchievedUnixMillis.value_or(score.timeAddedUnixMillis);
  const std::string remoteScoreId = score.remoteScoreId;
  ResultRecordSummary result{
      .identity = IrRemoteRecordId{
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
      .local = std::nullopt,
      .remote = std::move(score),
  };
  return result;
}

std::vector<ResultRecordSummary> mergeResultRecords(
    std::span<const ReplaySummary> local,
    std::span<const ir::IrRemoteScore> remote,
    std::string_view providerId, std::string_view serverOrigin) {
  std::unordered_set<std::string> linkedRemoteScoreIds;
  linkedRemoteScoreIds.reserve(local.size());
  for (const ReplaySummary &summary : local) {
    if (summary.hasIrReceipt && summary.receiptProviderId == providerId &&
        summary.receiptServerOrigin == serverOrigin &&
        !summary.receiptRemoteScoreId.empty()) {
      linkedRemoteScoreIds.emplace(summary.receiptRemoteScoreId);
    }
  }

  std::vector<ResultRecordSummary> result;
  result.reserve(local.size() + remote.size());
  for (const ReplaySummary &summary : local) {
    result.push_back(makeLocalResultRecord(summary));
  }
  for (const ir::IrRemoteScore &score : remote) {
    if (!linkedRemoteScoreIds.contains(score.remoteScoreId)) {
      result.push_back(
          makeRemoteResultRecord(providerId, serverOrigin, score));
    }
  }

  std::sort(result.begin(), result.end(),
            [](const ResultRecordSummary &left,
               const ResultRecordSummary &right) {
              if (left.autoPlay != right.autoPlay) {
                return left.autoPlay;
              }
              if (left.displayedTimeUnixMillis !=
                  right.displayedTimeUnixMillis) {
                return left.displayedTimeUnixMillis >
                       right.displayedTimeUnixMillis;
              }
              return left.stableKey() < right.stableKey();
            });
  return result;
}
