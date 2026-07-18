#include "IrRankingModal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace ir {
namespace {

// Eight wide columns need 886 px before giving Player any usable width.
constexpr int kCompactRowMaximumWidth = 999;
constexpr std::string_view kMissing = "\xE2\x80\x94";

std::string integerOrMissing(const std::optional<int> &value) {
  return value ? std::to_string(*value) : std::string(kMissing);
}

std::string rankText(int rank) {
  return rank > 0 ? "#" + std::to_string(rank) : std::string(kMissing);
}

std::string playerText(const IrChartRankingEntry &entry) {
  std::string value =
      entry.playerName.empty() ? std::string(kMissing) : entry.playerName;
  if (entry.currentUser) {
    value += "  \xC2\xB7  You";
  }
  return value;
}

std::string scoreText(int score, int maximum) {
  if (maximum <= 0) {
    return std::string(kMissing);
  }
  return std::to_string(score) + " / " + std::to_string(maximum);
}

void setFailure(IrRankingModalPresentation &presentation,
                IrRankingModalState state, std::string status,
                std::string detail, bool retry) {
  presentation.state = state;
  presentation.statusText = std::move(status);
  presentation.detailText = std::move(detail);
  presentation.canRefresh = true;
  presentation.canRetry = retry;
  presentation.ranking.reset();
  presentation.entryCount = 0;
  presentation.fetchedAtText.clear();
}

} // namespace

std::string formatIrRankingRate(int score, int maxScore) {
  if (maxScore <= 0) {
    return std::string(kMissing);
  }
  const double percentage =
      static_cast<double>(score) / static_cast<double>(maxScore) * 100.0;
  std::ostringstream output;
  output << std::fixed << std::setprecision(2) << percentage << '%';
  return output.str();
}

std::string formatIrRankingTimestamp(std::optional<std::int64_t> unixMillis) {
  if (!unixMillis || *unixMillis <= 0) {
    return std::string(kMissing);
  }
  const std::time_t seconds = static_cast<std::time_t>(*unixMillis / 1000);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &seconds) != 0) {
    return std::string(kMissing);
  }
#else
  if (gmtime_r(&seconds, &utc) == nullptr) {
    return std::string(kMissing);
  }
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%d %H:%M UTC");
  return output.str();
}

IrChartQueryBuildOutcome
makeBokutachiRankingQuery(const bms_parser::ChartMeta &meta) noexcept {
  try {
    std::string sha256 = meta.SHA256;
    sha256.erase(sha256.begin(),
                 std::find_if(sha256.begin(), sha256.end(), [](char value) {
                   return std::isspace(static_cast<unsigned char>(value)) == 0;
                 }));
    sha256.erase(std::find_if(sha256.rbegin(), sha256.rend(),
                              [](char value) {
                                return std::isspace(static_cast<unsigned char>(
                                           value)) == 0;
                              })
                     .base(),
                 sha256.end());
    std::ranges::transform(sha256, sha256.begin(), [](unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    const bool validSha =
        sha256.size() == 64 &&
        std::ranges::all_of(sha256, [](unsigned char value) {
          return std::isdigit(value) != 0 || (value >= 'a' && value <= 'f');
        });
    if ((meta.KeyMode != 7 && meta.KeyMode != 14) || meta.TotalNotes <= 0 ||
        meta.TotalNotes > std::numeric_limits<int>::max() / 2 || !validSha) {
      return {.diagnostic =
                  "Bokutachi rankings require a 7-key or 14-key chart with "
                  "positive notes and SHA-256 identity"};
    }
    std::string md5 = meta.MD5;
    std::ranges::transform(md5, md5.begin(), [](unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    const bool validMd5 =
        md5.size() == 32 && std::ranges::all_of(md5, [](unsigned char value) {
          return std::isdigit(value) != 0 || (value >= 'a' && value <= 'f');
        });
    return {.value = IrChartQuery{.keyMode = meta.KeyMode,
                                  .chartMd5 = validMd5 ? md5 : std::string{},
                                  .chartSha256 = std::move(sha256),
                                  .totalNotes = meta.TotalNotes}};
  } catch (...) {
    return {.diagnostic =
                "Bokutachi rankings require a 7-key or 14-key chart with "
                "positive notes and SHA-256 identity"};
  }
}

IrRankingPanelGeometry
layoutIrRankingPanel(const IrRankingPanelLayoutInput &input) noexcept {
  const int margin = std::max(0, input.margin);
  const int safeLeft = std::max(0, input.safeLeft);
  const int safeTop = std::max(0, input.safeTop);
  const int safeRight = std::max(0, input.safeRight);
  const int safeBottom = std::max(0, input.safeBottom);
  const int availableWidth =
      std::max(0, input.viewportWidth - safeLeft - safeRight - margin * 2);
  const int availableHeight =
      std::max(0, input.viewportHeight - safeTop - safeBottom - margin * 2);
  const int width = std::min(std::max(0, input.maximumWidth), availableWidth);
  const int height =
      std::min(std::max(0, input.maximumHeight), availableHeight);
  const int usableLeft = safeLeft + margin;
  const int usableTop = safeTop + margin;
  return {.x = usableLeft + (availableWidth - width) / 2,
          .y = usableTop + (availableHeight - height) / 2,
          .width = width,
          .height = height,
          .compact = width <= kCompactRowMaximumWidth + 44};
}

bool useCompactIrRankingColumns(int width) noexcept {
  return width <= kCompactRowMaximumWidth;
}

bool shouldLoadNextIrRankingPage(int entryCount, float scrollOffset,
                                 float viewportHeight, int itemHeight,
                                 int preloadRows) noexcept {
  if (entryCount <= 0 || scrollOffset < 0.0f || viewportHeight <= 0.0f ||
      itemHeight <= 0 || preloadRows < 0) {
    return false;
  }
  const float visibleBottom = scrollOffset + viewportHeight;
  const int visibleEnd =
      static_cast<int>(visibleBottom / static_cast<float>(itemHeight));
  return visibleEnd >= std::max(0, entryCount - preloadRows);
}

void IrRankingModalModel::open(IrRankingRequest request,
                               std::string chartTitle) {
  expectedRequest_ = std::move(request);
  presentation_ = {
      .state = IrRankingModalState::Loading,
      .chartTitle = std::move(chartTitle),
      .statusText = "Loading rankings...",
      .generation = expectedRequest_->generation,
      .comparison = expectedRequest_->localComparison,
  };
}

void IrRankingModalModel::refresh(std::uint64_t generation) {
  if (!expectedRequest_) {
    return;
  }
  expectedRequest_->generation = generation;
  presentation_.state = IrRankingModalState::Loading;
  presentation_.statusText = "Loading rankings...";
  presentation_.detailText.clear();
  presentation_.fetchedAtText.clear();
  presentation_.canRefresh = false;
  presentation_.canRetry = false;
  presentation_.entryCount = 0;
  presentation_.revision = 0;
  presentation_.generation = generation;
  presentation_.ranking.reset();
  presentation_.loadingNextPage = false;
  presentation_.canLoadNextPage = false;
  presentation_.paginationBlocked = false;
  presentation_.paginationStatusText.clear();
}

bool IrRankingModalModel::apply(const IrRankingSnapshot &snapshot) {
  if (!expectedRequest_ ||
      snapshot.generation != expectedRequest_->generation ||
      !snapshot.request || *snapshot.request != *expectedRequest_ ||
      (presentation_.revision != 0 &&
       snapshot.revision <= presentation_.revision)) {
    return false;
  }

  presentation_.revision = snapshot.revision;
  presentation_.generation = snapshot.generation;
  presentation_.detailText = snapshot.diagnostic;
  presentation_.comparison = expectedRequest_->localComparison;
  presentation_.comparisonInLeaderboard = false;
  presentation_.loadingNextPage = snapshot.loadingNextPage;
  presentation_.paginationBlocked = snapshot.paginationBlocked;
  presentation_.paginationStatusText.clear();

  switch (snapshot.state) {
  case IrRankingSnapshotState::Loading:
    presentation_.state = IrRankingModalState::Loading;
    presentation_.statusText = "Loading rankings...";
    presentation_.canRefresh = false;
    presentation_.canRetry = false;
    presentation_.ranking.reset();
    presentation_.entryCount = 0;
    presentation_.fetchedAtText.clear();
    presentation_.canLoadNextPage = false;
    break;
  case IrRankingSnapshotState::Succeeded:
    if (!snapshot.ranking || snapshot.ranking->entries.empty()) {
      presentation_.state = IrRankingModalState::Empty;
      presentation_.statusText = "No ranking entries yet";
      presentation_.canRefresh = true;
      presentation_.canRetry = true;
      presentation_.ranking = snapshot.ranking;
      presentation_.entryCount = 0;
      presentation_.fetchedAtText =
          snapshot.ranking
              ? formatIrRankingTimestamp(snapshot.ranking->fetchedAtUnixMillis)
              : std::string{};
      presentation_.canLoadNextPage = false;
      break;
    }
    presentation_.state = IrRankingModalState::Success;
    presentation_.statusText.clear();
    presentation_.detailText.clear();
    presentation_.canRefresh = true;
    presentation_.canRetry = false;
    presentation_.ranking = snapshot.ranking;
    presentation_.entryCount =
        static_cast<int>(snapshot.ranking->entries.size());
    presentation_.fetchedAtText =
        formatIrRankingTimestamp(snapshot.ranking->fetchedAtUnixMillis);
    presentation_.canLoadNextPage =
        snapshot.ranking->nextPageToken.has_value() &&
        !snapshot.loadingNextPage && !snapshot.paginationBlocked;
    if (snapshot.loadingNextPage) {
      presentation_.paginationStatusText = "Loading more rankings...";
    } else if (snapshot.paginationBlocked) {
      presentation_.paginationStatusText = snapshot.diagnostic.empty()
                                               ? "More rankings unavailable - "
                                                 "Refresh to retry"
                                               : "More rankings unavailable: " +
                                                     snapshot.diagnostic +
                                                     " - Refresh to retry";
      presentation_.detailText = snapshot.diagnostic;
    }
    break;
  case IrRankingSnapshotState::ChartNotFound:
    setFailure(presentation_, IrRankingModalState::NotFound,
               "No Bokutachi ranking for this chart", snapshot.diagnostic,
               true);
    break;
  case IrRankingSnapshotState::AuthenticationRequired:
    setFailure(presentation_, IrRankingModalState::AuthenticationRequired,
               "Authentication required",
               snapshot.diagnostic.empty()
                   ? "Add or replace the Bokutachi API key in IR settings."
                   : snapshot.diagnostic,
               true);
    break;
  case IrRankingSnapshotState::TransientFailure:
    setFailure(presentation_, IrRankingModalState::TransientFailure,
               "Bokutachi is unavailable or this device is offline",
               snapshot.diagnostic, true);
    break;
  case IrRankingSnapshotState::Unsupported:
    setFailure(presentation_, IrRankingModalState::Unsupported,
               "Rankings are unsupported", snapshot.diagnostic, false);
    break;
  case IrRankingSnapshotState::MalformedResponse:
    setFailure(presentation_, IrRankingModalState::Malformed,
               "Bokutachi returned an invalid response", snapshot.diagnostic,
               true);
    break;
  case IrRankingSnapshotState::OversizedResponse:
    setFailure(presentation_, IrRankingModalState::Oversized,
               "Bokutachi returned too much ranking data", snapshot.diagnostic,
               true);
    break;
  case IrRankingSnapshotState::Cancelled:
    setFailure(presentation_, IrRankingModalState::Cancelled,
               "Ranking request cancelled", snapshot.diagnostic, true);
    break;
  case IrRankingSnapshotState::Closed:
    return false;
  }
  return true;
}

IrRankingRowPresentation IrRankingModalModel::row(int index, int width) const {
  if (!presentation_.ranking || index < 0 ||
      index >= static_cast<int>(presentation_.ranking->entries.size())) {
    return {};
  }
  const auto &entry = presentation_.ranking->entries[index];
  const bool compact = useCompactIrRankingColumns(width);
  const bool showDetails = !compact;
  IrRankingRowPresentation value{
      .rankText = rankText(entry.rank),
      .playerText = playerText(entry),
      .scoreText = scoreText(entry.score, entry.maxScore),
      .rateText = formatIrRankingRate(entry.score, entry.maxScore),
      .lampText = clearTypeRankToLabel(entry.clearType),
      .badPointsText = integerOrMissing(entry.badPoints),
      .maxComboText = integerOrMissing(entry.maxCombo),
      .achievementTimeText =
          formatIrRankingTimestamp(entry.achievedAtUnixMillis),
      .clearType = entry.clearType,
      .highlighted = entry.currentUser,
      .compact = compact,
      .showBadPoints = showDetails,
      .showMaxCombo = showDetails,
      .showAchievementTime = showDetails,
  };
  return value;
}

std::optional<IrRankingScoreDetailPresentation>
IrRankingModalModel::scoreDetail(int index) const {
  if (!presentation_.ranking || index < 0 ||
      index >= static_cast<int>(presentation_.ranking->entries.size())) {
    return std::nullopt;
  }
  const auto &entry = presentation_.ranking->entries[index];
  const bool judgementBreakdownAvailable =
      entry.pGreat.has_value() || entry.great.has_value() ||
      entry.good.has_value() || entry.bad.has_value() ||
      entry.poor.has_value() || entry.earlyPGreat.has_value() ||
      entry.earlyGreat.has_value() || entry.earlyGood.has_value() ||
      entry.earlyBad.has_value() || entry.earlyPoor.has_value();
  return IrRankingScoreDetailPresentation{
      .rankText = rankText(entry.rank),
      .playerText = playerText(entry),
      .scoreText = scoreText(entry.score, entry.maxScore),
      .rateText = formatIrRankingRate(entry.score, entry.maxScore),
      .lampText = clearTypeRankToLabel(entry.clearType),
      .totalPGreatText = integerOrMissing(entry.pGreat),
      .totalGreatText = integerOrMissing(entry.great),
      .totalGoodText = integerOrMissing(entry.good),
      .totalBadText = integerOrMissing(entry.bad),
      .totalPoorText = integerOrMissing(entry.poor),
      .earlyPGreatText = integerOrMissing(entry.earlyPGreat),
      .latePGreatText = integerOrMissing(entry.latePGreat),
      .earlyGreatText = integerOrMissing(entry.earlyGreat),
      .lateGreatText = integerOrMissing(entry.lateGreat),
      .earlyGoodText = integerOrMissing(entry.earlyGood),
      .lateGoodText = integerOrMissing(entry.lateGood),
      .earlyBadText = integerOrMissing(entry.earlyBad),
      .lateBadText = integerOrMissing(entry.lateBad),
      .earlyPoorText = integerOrMissing(entry.earlyPoor),
      .latePoorText = integerOrMissing(entry.latePoor),
      .badPointsText = integerOrMissing(entry.badPoints),
      .maxComboText = integerOrMissing(entry.maxCombo),
      .achievementTimeText =
          formatIrRankingTimestamp(entry.achievedAtUnixMillis),
      .judgementBreakdownAvailable = judgementBreakdownAvailable,
      .clearType = entry.clearType,
      .highlighted = entry.currentUser,
  };
}

} // namespace ir
