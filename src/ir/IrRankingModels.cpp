#include "IrRankingModels.h"

#include "IrDriver.h"
#include "IrOutboxModels.h"
#include "IrProfileSettings.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <string_view>

namespace ir {
namespace {

std::string normalizedHash(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

bool isHexDigest(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

IrChartQueryBuildOutcome invalid(std::string_view diagnostic) {
  return {.diagnostic = sanitizeDiagnostic(diagnostic)};
}

} // namespace

IrChartQueryBuildOutcome
makeIrChartQuery(const bms_parser::ChartMeta &meta) noexcept {
  try {
    if (meta.KeyMode <= 0 || meta.TotalNotes <= 0 ||
        meta.TotalNotes > std::numeric_limits<int>::max() / 2) {
      return invalid("chart key mode or note count is invalid");
    }
    const std::string md5 = normalizedHash(meta.MD5);
    const std::string sha256 = normalizedHash(meta.SHA256);
    if ((!md5.empty() && !isHexDigest(md5, 32)) ||
        (!sha256.empty() && !isHexDigest(sha256, 64)) ||
        (md5.empty() && sha256.empty())) {
      return invalid("chart hash identity is malformed");
    }
    return {.value = IrChartQuery{.keyMode = meta.KeyMode,
                                  .chartMd5 = md5,
                                  .chartSha256 = sha256,
                                  .totalNotes = meta.TotalNotes}};
  } catch (...) {
    return invalid("chart ranking query construction failed");
  }
}

IrRankingCacheKeyBuildOutcome
makeIrRankingCacheKey(const IrRankingRequest &request) noexcept {
  try {
    const auto origin = normalizeServerOrigin(request.serverOrigin);
    const std::string sha256 = normalizedHash(request.chart.chartSha256);
    if (request.profileId.empty() || request.profileId.size() > 256 ||
        request.providerId.empty() || request.providerId.size() > 64 ||
        !origin || request.chart.keyMode <= 0 ||
        request.chart.totalNotes <= 0 ||
        request.chart.totalNotes > std::numeric_limits<int>::max() / 2 ||
        !isHexDigest(sha256, 64)) {
      return {.diagnostic = "ranking cache identity is invalid"};
    }
    return {.value = IrRankingCacheKey{.profileId = request.profileId,
                                       .providerId = request.providerId,
                                       .serverOrigin = *origin,
                                       .keyMode = request.chart.keyMode,
                                       .chartSha256 = sha256,
                                       .totalNotes = request.chart.totalNotes}};
  } catch (...) {
    return {.diagnostic = "ranking cache identity construction failed"};
  }
}

IrRankingSnapshotState snapshotStateFor(ChartRankingStatus status) noexcept {
  switch (status) {
  case ChartRankingStatus::Succeeded:
    return IrRankingSnapshotState::Succeeded;
  case ChartRankingStatus::ChartNotFound:
    return IrRankingSnapshotState::ChartNotFound;
  case ChartRankingStatus::AuthenticationRequired:
    return IrRankingSnapshotState::AuthenticationRequired;
  case ChartRankingStatus::TransientFailure:
    return IrRankingSnapshotState::TransientFailure;
  case ChartRankingStatus::Unsupported:
    return IrRankingSnapshotState::Unsupported;
  case ChartRankingStatus::MalformedResponse:
    return IrRankingSnapshotState::MalformedResponse;
  case ChartRankingStatus::OversizedResponse:
    return IrRankingSnapshotState::OversizedResponse;
  case ChartRankingStatus::Cancelled:
    return IrRankingSnapshotState::Cancelled;
  }
  return IrRankingSnapshotState::MalformedResponse;
}

std::string describeIrRankingCacheKey(const IrRankingCacheKey &key) {
  std::ostringstream output;
  output << "profile=" << key.profileId << ";provider=" << key.providerId
         << ";origin=" << key.serverOrigin << ";key_mode=" << key.keyMode
         << ";sha256=" << key.chartSha256 << ";notes=" << key.totalNotes;
  return output.str();
}

std::string describeIrChartRanking(const IrChartRanking &ranking) {
  std::ostringstream output;
  output << "provider=" << ranking.providerId
         << ";key_mode=" << ranking.chart.keyMode
         << ";sha256=" << ranking.chart.chartSha256
         << ";notes=" << ranking.chart.totalNotes
         << ";entries=" << ranking.entries.size()
         << ";fetched_at_ms=" << ranking.fetchedAtUnixMillis;
  return output.str();
}

} // namespace ir
