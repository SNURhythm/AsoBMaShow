#include "TachiDriver.h"

#include "../../CanonicalDigest.h"
#include "../../FileChecksum.h"
#include "../../ResultContracts.h"
#include "BokutachiCacheStore.h"
#include "TachiBatchManual.h"
#include "TachiRankingParser.h"
#include "TachiResponseParser.h"
#include "TachiUserScoreParser.h"
#include "../IrHttpClient.h"
#include "../IrProfileSettings.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ir::tachi {
namespace {

constexpr std::size_t kMaximumTachiResponseBytes = 1024 * 1024;
constexpr std::size_t kMaximumRankingPageTokenBytes = 2048;
constexpr std::size_t kMaximumRankingChartIdBytes = 256;
static_assert(BokutachiCacheStore::kMaximumBatchMappings >=
              kMaximumIrRemoteScoreSnapshotEntries);

DeliveryOutcome cancelled() {
  return {.status = DeliveryStatus::Cancelled,
          .code = "cancelled",
          .diagnostic = "Tachi request was cancelled"};
}

DeliveryOutcome blocked(std::string_view code, std::string_view diagnostic) {
  return {.status = DeliveryStatus::BlockedConfiguration,
          .code = std::string(code),
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

DeliveryOutcome permanent(std::string code, std::string diagnostic) {
  return {.status = DeliveryStatus::PermanentFailure,
          .code = std::move(code),
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

std::string proofFingerprintInput(const IrOutboxEntry &entry) {
  const auto &proof = entry.rulesetProof;
  std::string input = "tachi-lr2-proof-v1\n";
  input +=
      std::to_string(proof.rulesetId.size()) + ":" + proof.rulesetId + "\n";
  input += std::to_string(proof.rulesetRevision) + "\n";
  input +=
      std::to_string(entry.attemptId.size()) + ":" + entry.attemptId + "\n";
  input +=
      std::to_string(entry.chartSha256.size()) + ":" + entry.chartSha256 + "\n";
  input += std::to_string(entry.payloadJson.size()) + ":" + entry.payloadJson;
  return input;
}

std::optional<DeliveryOutcome>
invalidStoredRulesetProof(const IrOutboxEntry &entry) {
  const auto &proof = entry.rulesetProof;
  if (entry.providerId != kProviderId || proof.rulesetId != "lr2" ||
      proof.rulesetRevision != RulesetDescriptor::kCurrentVersion ||
      proof.validationFingerprint !=
          file_checksum::sha256(proofFingerprintInput(entry))) {
    return permanent("ruleset_proof_mismatch",
                     "The queued score ruleset proof is invalid.");
  }
  return std::nullopt;
}

std::optional<std::chrono::milliseconds>
parseRetryAfter(const std::optional<std::string> &value) {
  if (!value || value->empty()) {
    return std::nullopt;
  }
  std::uint64_t seconds = 0;
  const auto [end, error] =
      std::from_chars(value->data(), value->data() + value->size(), seconds);
  constexpr std::uint64_t maximumSeconds =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
      1000U;
  if (error != std::errc{} || end != value->data() + value->size() ||
      seconds > maximumSeconds) {
    return std::nullopt;
  }
  return std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000U));
}

std::string redact(std::string value, std::string_view apiKey) {
  if (!apiKey.empty()) {
    std::size_t offset = 0;
    while ((offset = value.find(apiKey, offset)) != std::string::npos) {
      value.replace(offset, apiKey.size(), "[redacted]");
      offset += 10;
    }
  }
  return sanitizeDiagnostic(value);
}

DeliveryOutcome redactOutcome(DeliveryOutcome outcome,
                              std::string_view apiKey) {
  outcome.diagnostic = redact(std::move(outcome.diagnostic), apiKey);
  return outcome;
}

DeliveryOutcome classifyFailure(const IrHttpResponse &response,
                                std::string_view apiKey) {
  if (response.transportError == IrTransportError::Cancelled) {
    return cancelled();
  }
  if (response.transportError != IrTransportError::None) {
    return {.status = DeliveryStatus::TransientFailure,
            .code = "transport_error",
            .diagnostic = redact(response.diagnostic.empty()
                                     ? "Tachi transport request failed"
                                     : response.diagnostic,
                                 apiKey)};
  }

  const auto status = response.statusCode;
  const std::string serverDescription = parseResponseDescription(response.body);
  const std::string diagnostic =
      serverDescription.empty()
          ? "Tachi request failed with HTTP " + std::to_string(status)
          : serverDescription;
  if (status == 401 || status == 403) {
    return blocked("authentication_failed", redact(diagnostic, apiKey));
  }
  if (status == 408 || status == 429 || status >= 500 || status <= 0) {
    return {.status = DeliveryStatus::TransientFailure,
            .retryAfterDelay = parseRetryAfter(response.retryAfter),
            .code = status > 0 ? "http_" + std::to_string(status)
                               : "invalid_http_status",
            .diagnostic = redact(diagnostic, apiKey)};
  }
  if (status >= 300 && status < 400) {
    return permanent("redirect_rejected", redact(diagnostic, apiKey));
  }
  return permanent("http_" + std::to_string(status),
                   redact(diagnostic, apiKey));
}

bool isHttpSuccess(long statusCode) {
  return statusCode >= 200 && statusCode < 300;
}

bool validCredential(std::string_view apiKey) {
  if (apiKey.empty() || apiKey.size() > 4 * 1024) {
    return false;
  }
  for (const unsigned char character : apiKey) {
    if (character <= 0x20U || character == 0x7fU) {
      return false;
    }
  }
  return true;
}

IrHttpRequest submitRequest(std::string_view origin, std::string body,
                            bool userIntent,
                            const IrProviderRuntimeConfig &config) {
  IrHttpRequest request{
      .method = IrHttpMethod::Post,
      .url = std::string(origin) + "/ir/direct-manual/import",
      .headers = {{"Authorization", "Bearer " + config.apiKey},
                  {"Content-Type", "application/json"}},
      .body = std::move(body),
      .maximumResponseBytes = kMaximumTachiResponseBytes,
      .followRedirects = false,
  };
  if (userIntent) {
    request.headers.emplace_back("X-User-Intent", "true");
  }
  return request;
}

DeliveryOutcome classifyBatchImport(std::span<const IrOutboxEntry> entries,
                                    DeliveryOutcome outcome) {
  if (entries.size() > 1 && outcome.importHadErrors) {
    return permanent("ambiguous_partial_import",
                     "Tachi returned an ambiguous partial batch result.");
  }
  if (outcome.remoteScoreIds.size() > entries.size()) {
    return permanent("malformed_response",
                     "Tachi returned more score IDs than submitted scores.");
  }
  return outcome;
}

ChartRankingOutcome rankingFailure(ChartRankingStatus status,
                                   std::string diagnostic,
                                   std::string_view apiKey = {}) {
  return {.status = status,
          .diagnostic = redact(std::move(diagnostic), apiKey)};
}

IrUserScoreSnapshotOutcome userScoreFailure(IrUserScoreSnapshotStatus status,
                                            std::string_view code,
                                            std::string diagnostic,
                                            std::string_view apiKey = {}) {
  return {.status = status,
          .code = std::string(code),
          .diagnostic = redact(std::move(diagnostic), apiKey)};
}

IrUserScoreSnapshotOutcome cancelledUserScore() {
  return userScoreFailure(IrUserScoreSnapshotStatus::Cancelled, "cancelled",
                          "Tachi user score request was cancelled");
}

IrUserScoreSnapshotOutcome
userScoreTransportFailure(const IrHttpResponse &response,
                          std::string_view apiKey) {
  if (response.transportError == IrTransportError::Cancelled) {
    return cancelledUserScore();
  }
  if (response.transportError == IrTransportError::ResponseTooLarge) {
    return userScoreFailure(
        IrUserScoreSnapshotStatus::OversizedResponse, "oversized_response",
        "Tachi user score response exceeded the size limit");
  }
  return userScoreFailure(
      IrUserScoreSnapshotStatus::TransientFailure, "transport_error",
      response.diagnostic.empty() ? "Tachi user score transport request failed"
                                  : response.diagnostic,
      apiKey);
}

void reportUserScoreProgress(const IrUserScoreProgress &progress,
                             std::string_view game, int completed) noexcept {
  if (!progress) {
    return;
  }
  try {
    progress(game, completed, 2);
  } catch (...) {
  }
}

std::optional<std::string> normalizedSha256(std::string_view value) {
  if (value.size() != 64) {
    return std::nullopt;
  }
  std::string normalized(value);
  std::ranges::transform(
      normalized, normalized.begin(), [](unsigned char character) {
        if (character >= 'A' && character <= 'F') {
          return static_cast<char>(character - 'A' + 'a');
        }
        return static_cast<char>(character);
      });
  if (!canonical_digest::isCanonicalLowerHex(normalized, 64)) {
    return std::nullopt;
  }
  return normalized;
}

std::string_view nativeBmsGame(int keyMode) {
  return keyMode == 14 ? "bms-14k" : "bms-7k";
}

std::string encodePathSegment(std::string_view value) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const unsigned char character : value) {
    const bool unreserved = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z') ||
                            (character >= '0' && character <= '9') ||
                            character == '-' || character == '_' ||
                            character == '.' || character == '~';
    if (unreserved) {
      encoded.push_back(static_cast<char>(character));
    } else {
      encoded.push_back('%');
      encoded.push_back(digits[character >> 4U]);
      encoded.push_back(digits[character & 0x0fU]);
    }
  }
  return encoded;
}

std::optional<ChartRankingOutcome>
rankingHttpFailure(const IrHttpResponse &response, std::string_view apiKey,
                   bool chartNotFoundOn404) {
  if (response.transportError == IrTransportError::Cancelled) {
    return rankingFailure(ChartRankingStatus::Cancelled,
                          "Tachi ranking request was cancelled");
  }
  if (response.transportError == IrTransportError::ResponseTooLarge) {
    return rankingFailure(ChartRankingStatus::OversizedResponse,
                          "Tachi ranking response exceeded the size limit");
  }
  if (response.transportError != IrTransportError::None) {
    return rankingFailure(ChartRankingStatus::TransientFailure,
                          response.diagnostic.empty()
                              ? "Tachi ranking transport request failed"
                              : response.diagnostic,
                          apiKey);
  }
  if (response.statusCode == 404 && chartNotFoundOn404) {
    return ChartRankingOutcome{.status = ChartRankingStatus::ChartNotFound};
  }
  if (response.statusCode == 401 || response.statusCode == 403) {
    return rankingFailure(ChartRankingStatus::AuthenticationRequired,
                          "Tachi ranking authentication failed");
  }
  if (response.statusCode == 408 || response.statusCode == 429 ||
      (response.statusCode >= 500 && response.statusCode < 600)) {
    return rankingFailure(ChartRankingStatus::TransientFailure,
                          "Tachi ranking request failed with HTTP " +
                              std::to_string(response.statusCode));
  }
  if (!isHttpSuccess(response.statusCode)) {
    return rankingFailure(ChartRankingStatus::MalformedResponse,
                          "Tachi ranking request failed with HTTP " +
                              std::to_string(response.statusCode));
  }
  return std::nullopt;
}

ChartRankingOutcome rankingParseFailure(ChartRankingStatus status,
                                        std::string diagnostic,
                                        std::string_view apiKey) {
  return rankingFailure(status, std::move(diagnostic), apiKey);
}

struct RankingPageCursor {
  std::string chartSha256;
  std::string chartId;
  std::optional<std::int64_t> userId;
  int startRanking = 0;
  int loadedEntries = 0;
  int lastRank = 0;
  int outOf = 0;
};

std::optional<std::int64_t> cursorInteger(const nlohmann::json &object,
                                          std::string_view key) {
  const auto found = object.find(key);
  if (found == object.end()) {
    return std::nullopt;
  }
  if (found->is_number_unsigned()) {
    const auto value = found->get<std::uint64_t>();
    if (value >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
  }
  if (!found->is_number_integer()) {
    return std::nullopt;
  }
  return found->get<std::int64_t>();
}

bool validCursorChartId(std::string_view value) {
  return !value.empty() && value.size() <= kMaximumRankingChartIdBytes &&
         std::ranges::none_of(value, [](unsigned char character) {
           return character <= 0x20U || character == 0x7fU;
         });
}

std::optional<RankingPageCursor>
parseRankingPageCursor(std::string_view token,
                       const IrChartQuery &normalizedQuery) {
  try {
    if (token.empty() || token.size() > kMaximumRankingPageTokenBytes) {
      return std::nullopt;
    }
    const auto document = nlohmann::json::parse(token);
    if (!document.is_object()) {
      return std::nullopt;
    }
    const auto version = cursorInteger(document, "v");
    const auto userIdValue = document.find("userID");
    std::optional<std::int64_t> userId;
    if (userIdValue != document.end() && !userIdValue->is_null()) {
      userId = cursorInteger(document, "userID");
    }
    const auto startRanking = cursorInteger(document, "startRanking");
    const auto loadedEntries = cursorInteger(document, "loadedEntries");
    const auto lastRank = cursorInteger(document, "lastRank");
    const auto outOf = cursorInteger(document, "outOf");
    const auto chartSha = document.find("chartSHA256");
    const auto chartId = document.find("chartID");
    if (!version || *version != 1 || userIdValue == document.end() ||
        (!userIdValue->is_null() && !userId) || (userId && *userId <= 0) ||
        !startRanking || *startRanking <= 1 ||
        *startRanking > std::numeric_limits<int>::max() || !loadedEntries ||
        *loadedEntries <= 0 ||
        *loadedEntries > static_cast<std::int64_t>(kMaximumRankingEntries) ||
        !lastRank || *lastRank <= 0 ||
        *lastRank > static_cast<std::int64_t>(kMaximumRankingEntries) ||
        !outOf || *outOf <= *loadedEntries ||
        *outOf > static_cast<std::int64_t>(kMaximumRankingEntries) ||
        *startRanking != *lastRank + 1 || *startRanking > *outOf ||
        *lastRank >= *outOf || chartSha == document.end() ||
        !chartSha->is_string() ||
        chartSha->get_ref<const std::string &>() !=
            normalizedQuery.chartSha256 ||
        chartId == document.end() || !chartId->is_string() ||
        !validCursorChartId(chartId->get_ref<const std::string &>())) {
      return std::nullopt;
    }
    return RankingPageCursor{
        .chartSha256 = chartSha->get<std::string>(),
        .chartId = chartId->get<std::string>(),
        .userId = userId,
        .startRanking = static_cast<int>(*startRanking),
        .loadedEntries = static_cast<int>(*loadedEntries),
        .lastRank = static_cast<int>(*lastRank),
        .outOf = static_cast<int>(*outOf),
    };
  } catch (...) {
    return std::nullopt;
  }
}

std::string makeRankingPageCursor(const IrChartQuery &query,
                                  std::string_view chartId,
                                  std::optional<std::int64_t> userId,
                                  int startRanking, int loadedEntries,
                                  int lastRank, int outOf) {
  return nlohmann::json{
      {"v", 1},
      {"chartSHA256", query.chartSha256},
      {"chartID", chartId},
      {"userID", userId ? nlohmann::json(*userId) : nlohmann::json(nullptr)},
      {"startRanking", startRanking},
      {"loadedEntries", loadedEntries},
      {"lastRank", lastRank},
      {"outOf", outOf}}
      .dump();
}

ChartRankingOutcome
fetchNativeRankingPage(const IrChartQuery &query, std::string_view origin,
                       std::string_view game, std::string_view chartId,
                       std::optional<std::int64_t> userId, int startRanking,
                       int loadedEntries, std::optional<int> expectedOutOf,
                       const IrProviderRuntimeConfig &config,
                       IrHttpClient &http, std::stop_token stopToken) {
  if (stopToken.stop_requested()) {
    return rankingFailure(ChartRankingStatus::Cancelled,
                          "Tachi ranking request was cancelled");
  }
  const IrHttpRequest pageRequest{
      .method = IrHttpMethod::Get,
      .url = std::string(origin) + "/api/v1/games/" + std::string(game) +
             "/charts/" + encodePathSegment(chartId) +
             "/pbs?startRanking=" + std::to_string(startRanking),
      .headers = {},
      .maximumResponseBytes = kMaximumRankingResponseBytes,
      .followRedirects = false,
  };
  const IrHttpResponse pageResponse = http.perform(pageRequest, stopToken);
  if (const auto failure =
          rankingHttpFailure(pageResponse, config.apiKey, true)) {
    return *failure;
  }
  auto page =
      parseRankingPageResponse(pageResponse.body, query, chartId, userId);
  if (page.status != ChartRankingStatus::Succeeded || !page.page) {
    return rankingParseFailure(page.status, page.diagnostic, config.apiKey);
  }
  if (page.page->entries.empty()) {
    if (loadedEntries != 0 || expectedOutOf) {
      return rankingFailure(ChartRankingStatus::MalformedResponse,
                            "Tachi ranking continuation ended early");
    }
    return {.status = ChartRankingStatus::Succeeded,
            .ranking = IrChartRanking{.providerId = std::string(kProviderId),
                                      .chart = query}};
  }
  if (page.page->entries.front().rank < startRanking ||
      (expectedOutOf && page.page->outOf != *expectedOutOf)) {
    return rankingFailure(ChartRankingStatus::MalformedResponse,
                          "Tachi ranking page cursor does not match response");
  }
  const int newLoadedEntries =
      loadedEntries + static_cast<int>(page.page->entries.size());
  if (page.page->outOf <= 0 || newLoadedEntries > page.page->outOf) {
    return rankingFailure(ChartRankingStatus::MalformedResponse,
                          "Tachi ranking page count is invalid");
  }

  IrChartRanking ranking{.providerId = std::string(kProviderId),
                         .chart = query,
                         .entries = std::move(page.page->entries)};
  if (newLoadedEntries < page.page->outOf) {
    const int nextStart = ranking.entries.back().rank + 1;
    if (nextStart <= startRanking || nextStart > page.page->outOf) {
      return rankingFailure(ChartRankingStatus::MalformedResponse,
                            "Tachi ranking page did not advance");
    }
    ranking.nextPageToken = makeRankingPageCursor(
        query, chartId, userId, nextStart, newLoadedEntries,
        ranking.entries.back().rank, page.page->outOf);
  }
  return {.status = ChartRankingStatus::Succeeded,
          .ranking = std::move(ranking)};
}

} // namespace

TachiDriver::TachiDriver(
    std::shared_ptr<BokutachiCacheStore> cacheStore) noexcept
    : cacheStore_(std::move(cacheStore)) {}

std::string_view TachiDriver::providerId() const noexcept {
  return kProviderId;
}

IrDriverCapabilities TachiDriver::capabilities() const noexcept {
  return {.readOnly = false,
          .chartRankings = true,
          .scoreSubmission = true,
          .deferredSubmission = true,
          .scoreReconciliation = true};
}

BuildDraftOutcome
TachiDriver::buildDraft(const IrSubmission &submission) const {
  return buildBatchManualDraft(submission);
}

IrOutboxBatchPlan
TachiDriver::planBatch(std::span<const IrOutboxEntry> due) const {
  if (due.empty()) {
    return {.status = IrOutboxBatchPlanStatus::Invalid,
            .diagnostic = "Tachi batch has no due rows"};
  }
  const auto &first = due.front();
  if (const auto invalidProof = invalidStoredRulesetProof(first)) {
    return {.status = IrOutboxBatchPlanStatus::Invalid,
            .rejectedRowId = first.id,
            .diagnostic = invalidProof->diagnostic};
  }
  if (first.state != IrOutboxState::AwaitingRemoteResult) {
    const auto built = buildBatchManualOutboxDocument(due);
    if (built.status != BuildTachiOutboxBatchStatus::Built || !built.document) {
      return {.status = IrOutboxBatchPlanStatus::Invalid,
              .rejectedRowId = built.rejectedRowId,
              .diagnostic = built.diagnostic};
    }
    return {.status = IrOutboxBatchPlanStatus::Planned,
            .rowIds = built.document->rowIds};
  }

  const auto firstOrigin = normalizeServerOrigin(first.remoteOrigin);
  if (!firstOrigin || !isValidImportId(first.remoteJobId)) {
    return {.status = IrOutboxBatchPlanStatus::Invalid,
            .rejectedRowId = first.id,
            .diagnostic = "Tachi deferred import state is missing or invalid"};
  }
  std::vector<std::int64_t> rowIds;
  rowIds.reserve(std::min<std::size_t>(due.size(), 64));
  for (const auto &entry : due) {
    if (rowIds.size() == 64) {
      break;
    }
    if (entry.state != IrOutboxState::AwaitingRemoteResult ||
        entry.remoteJobId != first.remoteJobId) {
      continue;
    }
    const auto origin = normalizeServerOrigin(entry.remoteOrigin);
    if (!origin || *origin != *firstOrigin) {
      continue;
    }
    if (const auto invalidProof = invalidStoredRulesetProof(entry)) {
      if (rowIds.empty()) {
        return {.status = IrOutboxBatchPlanStatus::Invalid,
                .rejectedRowId = entry.id,
                .diagnostic = invalidProof->diagnostic};
      }
      break;
    }
    rowIds.push_back(entry.id);
  }
  return {.status = IrOutboxBatchPlanStatus::Planned,
          .rowIds = std::move(rowIds)};
}

DeliveryOutcome TachiDriver::submit(const IrOutboxEntry &entry,
                                    const IrProviderRuntimeConfig &config,
                                    IrHttpClient &http,
                                    std::stop_token stopToken) const {
  return submitBatch(std::span<const IrOutboxEntry>(&entry, 1),
                     entry.nextRequestUserIntent, config, http, stopToken);
}

DeliveryOutcome TachiDriver::submitBatch(std::span<const IrOutboxEntry> entries,
                                         bool userIntent,
                                         const IrProviderRuntimeConfig &config,
                                         IrHttpClient &http,
                                         std::stop_token stopToken) const {
  if (stopToken.stop_requested()) {
    return cancelled();
  }
  if (entries.empty() || entries.size() > 64) {
    return permanent("invalid_batch", "Tachi submission batch is invalid");
  }
  for (const auto &entry : entries) {
    if (const auto invalidProof = invalidStoredRulesetProof(entry)) {
      return *invalidProof;
    }
  }
  if (entries.front().state == IrOutboxState::AwaitingRemoteResult) {
    return pollBatch(entries, config, http, stopToken);
  }
  if (!validCredential(config.apiKey)) {
    return blocked("missing_api_key", "Tachi API key is missing or invalid");
  }
  const auto origin = normalizeServerOrigin(config.serverOrigin);
  if (!origin) {
    return blocked("invalid_server_origin",
                   "Tachi server origin is missing or invalid");
  }
  if (!isHttpsServerOrigin(*origin)) {
    return blocked("insecure_server_origin",
                   "Authenticated Tachi submission requires HTTPS");
  }
  const auto built = buildBatchManualOutboxDocument(entries);
  if (built.status != BuildTachiOutboxBatchStatus::Built || !built.document ||
      built.document->rowIds.size() != entries.size()) {
    return permanent("invalid_outbox_entry",
                     built.diagnostic.empty()
                         ? "Tachi outbox batch is incompatible or invalid"
                         : built.diagnostic);
  }

  const IrHttpResponse response = http.perform(
      submitRequest(*origin, built.document->payloadJson, userIntent, config),
      stopToken);
  if (response.transportError != IrTransportError::None ||
      !isHttpSuccess(response.statusCode)) {
    return classifyFailure(response, config.apiKey);
  }
  DeliveryOutcome outcome =
      response.statusCode == 202
          ? parseDeferredImportResponse(response.body, *origin)
          : parseImmediateImportResponse(response.body);
  return redactOutcome(classifyBatchImport(entries, std::move(outcome)),
                       config.apiKey);
}

DeliveryOutcome TachiDriver::poll(const IrOutboxEntry &entry,
                                  const IrProviderRuntimeConfig &config,
                                  IrHttpClient &http,
                                  std::stop_token stopToken) const {
  return pollBatch(std::span<const IrOutboxEntry>(&entry, 1), config, http,
                   stopToken);
}

DeliveryOutcome TachiDriver::pollBatch(std::span<const IrOutboxEntry> entries,
                                       const IrProviderRuntimeConfig &config,
                                       IrHttpClient &http,
                                       std::stop_token stopToken) const {
  if (stopToken.stop_requested()) {
    return cancelled();
  }
  if (entries.empty() || entries.size() > 64) {
    return permanent("invalid_batch", "Tachi polling batch is invalid");
  }
  for (const auto &entry : entries) {
    if (const auto invalidProof = invalidStoredRulesetProof(entry)) {
      return *invalidProof;
    }
  }
  if (!validCredential(config.apiKey)) {
    return blocked("missing_api_key", "Tachi API key is missing or invalid");
  }
  const auto &first = entries.front();
  const auto origin = normalizeServerOrigin(first.remoteOrigin);
  if (!origin || !isValidImportId(first.remoteJobId)) {
    return permanent("invalid_remote_job",
                     "Tachi deferred import state is missing or invalid");
  }
  if (!isHttpsServerOrigin(*origin)) {
    return blocked("insecure_server_origin",
                   "Authenticated Tachi polling requires HTTPS");
  }
  for (const auto &entry : entries.subspan(1)) {
    const auto entryOrigin = normalizeServerOrigin(entry.remoteOrigin);
    if (entry.remoteJobId != first.remoteJobId || !entryOrigin ||
        *entryOrigin != *origin) {
      return permanent("invalid_remote_job",
                       "Tachi deferred batch state does not match");
    }
  }

  const IrHttpRequest request{
      .method = IrHttpMethod::Get,
      .url = *origin + "/api/v1/imports/" + first.remoteJobId + "/poll-status",
      .headers = {{"Authorization", "Bearer " + config.apiKey}},
      .maximumResponseBytes = kMaximumTachiResponseBytes,
      .followRedirects = false,
  };
  const IrHttpResponse response = http.perform(request, stopToken);
  if (response.transportError != IrTransportError::None ||
      !isHttpSuccess(response.statusCode)) {
    return classifyFailure(response, config.apiKey);
  }
  return redactOutcome(
      classifyBatchImport(entries, parsePollStatusResponse(response.body)),
      config.apiKey);
}

ChartRankingOutcome TachiDriver::fetchChartRanking(
    const IrChartQuery &query, const IrProviderRuntimeConfig &config,
    IrHttpClient &http, std::stop_token stopToken) const {
  if (stopToken.stop_requested()) {
    return rankingFailure(ChartRankingStatus::Cancelled,
                          "Tachi ranking request was cancelled");
  }
  if (query.keyMode != 7 && query.keyMode != 14) {
    return rankingFailure(ChartRankingStatus::Unsupported,
                          "Bokutachi supports BMS 7K and 14K rankings only");
  }
  const bool authenticated = !config.apiKey.empty();
  if (authenticated && !validCredential(config.apiKey)) {
    return rankingFailure(ChartRankingStatus::AuthenticationRequired,
                          "Tachi API key is invalid");
  }
  const auto origin = normalizeServerOrigin(config.serverOrigin);
  if (!origin) {
    return rankingFailure(ChartRankingStatus::MalformedResponse,
                          "Tachi server origin is missing or invalid");
  }
  if (authenticated && !isHttpsServerOrigin(*origin)) {
    return rankingFailure(ChartRankingStatus::AuthenticationRequired,
                          "Authenticated Tachi ranking requires HTTPS");
  }
  const auto sha256 = normalizedSha256(query.chartSha256);
  if (!sha256 || query.totalNotes <= 0 ||
      !result_contract::maximumScoreForNotes(query.totalNotes)) {
    return rankingFailure(ChartRankingStatus::MalformedResponse,
                          "Tachi ranking chart query is invalid");
  }

  IrChartQuery normalizedQuery = query;
  normalizedQuery.chartSha256 = *sha256;
  const std::string game(nativeBmsGame(query.keyMode));
  std::optional<std::string> chartId =
      cacheStore_ ? cacheStore_->chartId(*origin, game, *sha256) : std::nullopt;
  const bool usedCachedChartId = chartId.has_value();
  auto resolveChart = [&]() -> std::optional<ChartRankingOutcome> {
    if (stopToken.stop_requested()) {
      return rankingFailure(ChartRankingStatus::Cancelled,
                            "Tachi ranking request was cancelled");
    }
    const IrHttpRequest resolveRequest{
        .method = IrHttpMethod::Post,
        .url = *origin + "/api/v1/games/" + game + "/charts/resolve",
        .headers = {{"Content-Type", "application/json"}},
        .body = "{\"identifier\":\"" + *sha256 +
                "\",\"matchType\":\"bmsChartHash\"}",
        .maximumResponseBytes = kMaximumTachiResponseBytes,
        .followRedirects = false,
    };
    const IrHttpResponse response = http.perform(resolveRequest, stopToken);
    if (const auto failure =
            rankingHttpFailure(response, config.apiKey, true)) {
      return *failure;
    }
    const auto parsed =
        parseChartResolveResponse(response.body, normalizedQuery);
    if (parsed.status != ChartRankingStatus::Succeeded || !parsed.chartId) {
      return rankingParseFailure(parsed.status, parsed.diagnostic,
                                 config.apiKey);
    }
    if (stopToken.stop_requested()) {
      return rankingFailure(ChartRankingStatus::Cancelled,
                            "Tachi ranking request was cancelled");
    }
    chartId = *parsed.chartId;
    if (cacheStore_) {
      std::string ignoredDiagnostic;
      (void)cacheStore_->rememberChartId(*origin, game, *sha256, *chartId,
                                         ignoredDiagnostic);
    }
    return std::nullopt;
  };

  if (!chartId) {
    if (const auto failure = resolveChart()) {
      return *failure;
    }
  }

  std::optional<std::int64_t> userId;
  if (authenticated) {
    userId = cacheStore_ ? cacheStore_->userId(*origin) : std::nullopt;
  }
  if (authenticated && !userId) {
    if (stopToken.stop_requested()) {
      return rankingFailure(ChartRankingStatus::Cancelled,
                            "Tachi ranking request was cancelled");
    }
    const IrHttpRequest identityRequest{
        .method = IrHttpMethod::Get,
        .url = *origin + "/api/v1/status",
        .headers = {{"Authorization", "Bearer " + config.apiKey}},
        .maximumResponseBytes = kMaximumTachiResponseBytes,
        .followRedirects = false,
    };
    const IrHttpResponse identityResponse =
        http.perform(identityRequest, stopToken);
    if (const auto failure =
            rankingHttpFailure(identityResponse, config.apiKey, false)) {
      return *failure;
    }
    const auto identity = parseRankingIdentityResponse(identityResponse.body);
    if (identity.status != ChartRankingStatus::Succeeded || !identity.userId) {
      return rankingParseFailure(identity.status, identity.diagnostic,
                                 config.apiKey);
    }
    if (stopToken.stop_requested()) {
      return rankingFailure(ChartRankingStatus::Cancelled,
                            "Tachi ranking request was cancelled");
    }
    userId = *identity.userId;
    if (cacheStore_) {
      std::string ignoredDiagnostic;
      (void)cacheStore_->rememberUserId(*origin, *userId, ignoredDiagnostic);
    }
  }

  auto page =
      fetchNativeRankingPage(normalizedQuery, *origin, game, *chartId, userId,
                             1, 0, std::nullopt, config, http, stopToken);
  if (page.status != ChartRankingStatus::ChartNotFound || !usedCachedChartId) {
    return page;
  }

  if (cacheStore_) {
    std::string ignoredDiagnostic;
    (void)cacheStore_->eraseChartId(*origin, game, *sha256, ignoredDiagnostic);
  }
  chartId.reset();
  if (const auto failure = resolveChart()) {
    return *failure;
  }
  return fetchNativeRankingPage(normalizedQuery, *origin, game, *chartId,
                                userId, 1, 0, std::nullopt, config, http,
                                stopToken);
}

ChartRankingOutcome TachiDriver::fetchChartRankingPage(
    const IrChartQuery &query, std::string_view pageToken,
    const IrProviderRuntimeConfig &config, IrHttpClient &http,
    std::stop_token stopToken) const {
  if (stopToken.stop_requested()) {
    return rankingFailure(ChartRankingStatus::Cancelled,
                          "Tachi ranking request was cancelled");
  }
  if (query.keyMode != 7 && query.keyMode != 14) {
    return rankingFailure(ChartRankingStatus::Unsupported,
                          "Bokutachi supports BMS 7K and 14K rankings only");
  }
  const auto origin = normalizeServerOrigin(config.serverOrigin);
  const auto sha256 = normalizedSha256(query.chartSha256);
  if (!origin || !sha256 || query.totalNotes <= 0 ||
      !result_contract::maximumScoreForNotes(query.totalNotes)) {
    return rankingFailure(ChartRankingStatus::MalformedResponse,
                          "Tachi ranking continuation request is invalid");
  }
  IrChartQuery normalizedQuery = query;
  normalizedQuery.chartSha256 = *sha256;
  const auto cursor = parseRankingPageCursor(pageToken, normalizedQuery);
  if (!cursor) {
    return rankingFailure(ChartRankingStatus::MalformedResponse,
                          "Tachi ranking continuation token is invalid");
  }
  return fetchNativeRankingPage(
      normalizedQuery, *origin, nativeBmsGame(query.keyMode), cursor->chartId,
      cursor->userId, cursor->startRanking, cursor->loadedEntries,
      cursor->outOf, config, http, stopToken);
}

IrUserScoreSnapshotOutcome TachiDriver::fetchUserScoreSnapshot(
    const IrProviderRuntimeConfig &config, IrHttpClient &http,
    std::stop_token stopToken, IrUserScoreProgress progress) const {
  if (stopToken.stop_requested()) {
    return cancelledUserScore();
  }
  if (!validCredential(config.apiKey)) {
    return userScoreFailure(IrUserScoreSnapshotStatus::AuthenticationRequired,
                            "authentication_required",
                            "Tachi API key is missing or invalid");
  }
  const auto origin = normalizeServerOrigin(config.serverOrigin);
  if (!origin) {
    return userScoreFailure(IrUserScoreSnapshotStatus::MalformedResponse,
                            "invalid_server_origin",
                            "Tachi server origin is missing or invalid");
  }
  if (!isHttpsServerOrigin(*origin)) {
    return userScoreFailure(IrUserScoreSnapshotStatus::AuthenticationRequired,
                            "insecure_server_origin",
                            "Authenticated Tachi score import requires HTTPS");
  }

  constexpr std::array games{std::string_view("bms-7k"),
                             std::string_view("bms-14k")};
  std::array<IrUserScoreSnapshotOutcome, games.size()> outcomes;
  std::optional<std::size_t> firstFailure;
  for (std::size_t index = 0; index < games.size(); ++index) {
    if (stopToken.stop_requested()) {
      return cancelledUserScore();
    }
    reportUserScoreProgress(progress, games[index], static_cast<int>(index));
    if (stopToken.stop_requested()) {
      return cancelledUserScore();
    }
    const IrHttpRequest request{
        .method = IrHttpMethod::Get,
        .url = *origin + "/api/v1/users/me/games/" + std::string(games[index]) +
               "/scores/all",
        .headers = {{"Authorization", "Bearer " + config.apiKey}},
        .maximumResponseBytes = kMaximumTachiUserScoreResponseBytes,
        .followRedirects = false,
    };
    const IrHttpResponse response = http.perform(request, stopToken);
    reportUserScoreProgress(progress, games[index],
                            static_cast<int>(index + 1));
    if (response.transportError == IrTransportError::Cancelled ||
        stopToken.stop_requested()) {
      return cancelledUserScore();
    }
    outcomes[index] =
        response.transportError == IrTransportError::None
            ? parseUserGameScores(games[index], response.statusCode,
                                  response.body)
            : userScoreTransportFailure(response, config.apiKey);
    outcomes[index].diagnostic =
        redact(std::move(outcomes[index].diagnostic), config.apiKey);
    if (outcomes[index].status == IrUserScoreSnapshotStatus::Succeeded &&
        !outcomes[index].snapshot) {
      outcomes[index] = userScoreFailure(
          IrUserScoreSnapshotStatus::MalformedResponse, "malformed_response",
          "Tachi user score parser returned an incomplete result");
    }
    if (outcomes[index].status != IrUserScoreSnapshotStatus::Succeeded) {
      firstFailure = firstFailure.value_or(index);
    }
  }

  if (stopToken.stop_requested()) {
    return cancelledUserScore();
  }
  if (firstFailure) {
    outcomes[*firstFailure].snapshot.reset();
    return std::move(outcomes[*firstFailure]);
  }

  IrUserScoreSnapshot merged;
  const std::size_t mergedSize =
      outcomes[0].snapshot->scores.size() + outcomes[1].snapshot->scores.size();
  if (mergedSize > kMaximumIrRemoteScoreSnapshotEntries) {
    return userScoreFailure(IrUserScoreSnapshotStatus::MalformedResponse,
                            "malformed_response",
                            "Tachi combined user score snapshot is oversized");
  }
  merged.scores.reserve(mergedSize);
  for (auto &outcome : outcomes) {
    std::ranges::move(outcome.snapshot->scores,
                      std::back_inserter(merged.scores));
  }

  std::optional<std::int64_t> userId;
  for (const auto &score : merged.scores) {
    if (userId && *userId != score.remoteUserId) {
      return userScoreFailure(
          IrUserScoreSnapshotStatus::MalformedResponse, "malformed_response",
          "Tachi user score games disagree on user identity");
    }
    userId = score.remoteUserId;
  }
  std::string diagnostic;
  if (!validateIrUserScoreSnapshot(merged, diagnostic)) {
    return userScoreFailure(IrUserScoreSnapshotStatus::MalformedResponse,
                            "malformed_response", std::move(diagnostic));
  }
  if (stopToken.stop_requested()) {
    return cancelledUserScore();
  }

  if (cacheStore_ && (!merged.scores.empty() || userId)) {
    std::vector<BokutachiChartMapping> mappings;
    mappings.reserve(merged.scores.size());
    for (const auto &score : merged.scores) {
      mappings.push_back({.game = score.game,
                          .chartSha256 = score.chartSha256,
                          .chartId = score.remoteChartId});
    }
    std::string ignoredDiagnostic;
    (void)cacheStore_->rememberSnapshot(*origin, userId, mappings,
                                        ignoredDiagnostic);
  }
  return {.status = IrUserScoreSnapshotStatus::Succeeded,
          .snapshot = std::move(merged)};
}

} // namespace ir::tachi
