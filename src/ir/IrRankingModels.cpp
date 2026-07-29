#include "IrRankingModels.h"

#include "../BmsMetadataText.h"
#include "../CanonicalDigest.h"
#include "../ResultContracts.h"
#include "IrDriver.h"
#include "IrOutboxModels.h"
#include "IrProfileSettings.h"

#include <limits>
#include <sstream>
#include <string_view>

namespace ir {
namespace {

IrChartQueryBuildOutcome invalid(std::string_view diagnostic) {
  return {.diagnostic = sanitizeDiagnostic(diagnostic)};
}

} // namespace

IrChartQueryBuildOutcome
makeIrChartQuery(const bms_parser::ChartMeta &meta) noexcept {
  try {
    if (meta.KeyMode <= 0 || meta.TotalNotes <= 0 ||
        !result_contract::maximumScoreForNotes(meta.TotalNotes)) {
      return invalid("chart key mode or note count is invalid");
    }
    const std::string md5 =
        asobmshow::bms_metadata::normalizedHash(meta.MD5);
    const std::string sha256 =
        asobmshow::bms_metadata::normalizedHash(meta.SHA256);
    if ((!md5.empty() &&
         !canonical_digest::isCanonicalLowerHex(md5, 32)) ||
        (!sha256.empty() &&
         !canonical_digest::isCanonicalLowerHex(sha256, 64)) ||
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

std::optional<int> calculateIrBadPoints(int bad, int poor, int kPoor) noexcept {
  if (bad < 0 || poor < 0 || kPoor < 0) {
    return std::nullopt;
  }
  const long long total = static_cast<long long>(bad) + poor + kPoor;
  if (total > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(total);
}

IrRankingCacheKeyBuildOutcome
makeIrRankingCacheKey(const IrRankingRequest &request) noexcept {
  try {
    const auto origin = normalizeServerOrigin(request.serverOrigin);
    const std::string sha256 = asobmshow::bms_metadata::normalizedHash(
        request.chart.chartSha256);
    if (request.profileId.empty() || request.profileId.size() > 256 ||
        !ir::isValidProviderId(request.providerId) ||
        !origin || request.chart.keyMode <= 0 ||
        request.chart.totalNotes <= 0 ||
        !result_contract::maximumScoreForNotes(request.chart.totalNotes) ||
        !canonical_digest::isCanonicalLowerHex(sha256, 64)) {
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
