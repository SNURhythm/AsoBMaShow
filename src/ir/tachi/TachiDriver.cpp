#include "TachiDriver.h"

#include "../../FileChecksum.h"
#include "TachiBatchManual.h"
#include "TachiRankingParser.h"
#include "TachiResponseParser.h"
#include "../IrHttpClient.h"
#include "../IrProfileSettings.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <string>

namespace ir::tachi {
namespace {

constexpr std::size_t kMaximumTachiResponseBytes = 1024 * 1024;

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

IrHttpRequest submitRequest(std::string_view origin, const IrOutboxEntry &entry,
                            const IrProviderRuntimeConfig &config) {
  IrHttpRequest request{
      .method = IrHttpMethod::Post,
      .url = std::string(origin) + "/ir/direct-manual/import",
      .headers = {{"Authorization", "Bearer " + config.apiKey},
                  {"Content-Type", "application/json"}},
      .body = entry.payloadJson,
      .maximumResponseBytes = kMaximumTachiResponseBytes,
      .followRedirects = false,
  };
  if (entry.nextRequestUserIntent) {
    request.headers.emplace_back("X-User-Intent", "true");
  }
  return request;
}

ChartRankingOutcome rankingFailure(ChartRankingStatus status,
                                   std::string diagnostic,
                                   std::string_view apiKey = {}) {
  return {.status = status,
          .diagnostic = redact(std::move(diagnostic), apiKey)};
}

std::optional<std::string> normalizedSha256(std::string_view value) {
  if (value.size() != 64) {
    return std::nullopt;
  }
  std::string normalized;
  normalized.reserve(value.size());
  for (const unsigned char character : value) {
    if ((character >= '0' && character <= '9') ||
        (character >= 'a' && character <= 'f')) {
      normalized.push_back(static_cast<char>(character));
    } else if (character >= 'A' && character <= 'F') {
      normalized.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      return std::nullopt;
    }
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

} // namespace

std::string_view TachiDriver::providerId() const noexcept {
  return kProviderId;
}

IrDriverCapabilities TachiDriver::capabilities() const noexcept {
  return {.readOnly = false,
          .chartRankings = true,
          .scoreSubmission = true,
          .deferredSubmission = true};
}

BuildDraftOutcome
TachiDriver::buildDraft(const IrSubmission &submission) const {
  return buildBatchManualDraft(submission);
}

DeliveryOutcome TachiDriver::submit(const IrOutboxEntry &entry,
                                    const IrProviderRuntimeConfig &config,
                                    IrHttpClient &http,
                                    std::stop_token stopToken) const {
  if (stopToken.stop_requested()) {
    return cancelled();
  }
  if (const auto invalidProof = invalidStoredRulesetProof(entry)) {
    return *invalidProof;
  }
  if (entry.state == IrOutboxState::AwaitingRemoteResult) {
    return poll(entry, config, http, stopToken);
  }
  if (!validCredential(config.apiKey)) {
    return blocked("missing_api_key", "Tachi API key is missing or invalid");
  }
  const auto origin = normalizeServerOrigin(config.serverOrigin);
  if (!origin) {
    return blocked("invalid_server_origin",
                   "Tachi server origin is missing or invalid");
  }
  if (entry.providerId != kProviderId || entry.payloadJson.empty() ||
      entry.payloadJson.size() > kMaximumIrPayloadBytes) {
    return permanent("invalid_outbox_entry",
                     "Tachi outbox payload is missing or invalid");
  }

  const IrHttpResponse response =
      http.perform(submitRequest(*origin, entry, config), stopToken);
  if (response.transportError != IrTransportError::None ||
      !isHttpSuccess(response.statusCode)) {
    return classifyFailure(response, config.apiKey);
  }
  DeliveryOutcome outcome =
      response.statusCode == 202
          ? parseDeferredImportResponse(response.body, *origin)
          : parseImmediateImportResponse(response.body);
  return redactOutcome(std::move(outcome), config.apiKey);
}

DeliveryOutcome TachiDriver::poll(const IrOutboxEntry &entry,
                                  const IrProviderRuntimeConfig &config,
                                  IrHttpClient &http,
                                  std::stop_token stopToken) const {
  if (stopToken.stop_requested()) {
    return cancelled();
  }
  if (const auto invalidProof = invalidStoredRulesetProof(entry)) {
    return *invalidProof;
  }
  if (!validCredential(config.apiKey)) {
    return blocked("missing_api_key", "Tachi API key is missing or invalid");
  }
  const auto origin = normalizeServerOrigin(entry.remoteOrigin);
  if (!origin || !isValidImportId(entry.remoteJobId)) {
    return permanent("invalid_remote_job",
                     "Tachi deferred import state is missing or invalid");
  }

  const IrHttpRequest request{
      .method = IrHttpMethod::Get,
      .url = *origin + "/api/v1/imports/" + entry.remoteJobId + "/poll-status",
      .headers = {{"Authorization", "Bearer " + config.apiKey}},
      .maximumResponseBytes = kMaximumTachiResponseBytes,
      .followRedirects = false,
  };
  const IrHttpResponse response = http.perform(request, stopToken);
  if (response.transportError != IrTransportError::None ||
      !isHttpSuccess(response.statusCode)) {
    return classifyFailure(response, config.apiKey);
  }
  return redactOutcome(parsePollStatusResponse(response.body), config.apiKey);
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
  if (!validCredential(config.apiKey)) {
    return rankingFailure(ChartRankingStatus::AuthenticationRequired,
                          "Tachi API key is missing or invalid");
  }
  const auto origin = normalizeServerOrigin(config.serverOrigin);
  if (!origin) {
    return rankingFailure(ChartRankingStatus::MalformedResponse,
                          "Tachi server origin is missing or invalid");
  }
  const auto sha256 = normalizedSha256(query.chartSha256);
  if (!sha256 || query.totalNotes <= 0 ||
      query.totalNotes > std::numeric_limits<int>::max() / 2) {
    return rankingFailure(ChartRankingStatus::MalformedResponse,
                          "Tachi ranking chart query is invalid");
  }

  IrChartQuery normalizedQuery = query;
  normalizedQuery.chartSha256 = *sha256;
  const std::string game(nativeBmsGame(query.keyMode));
  const std::vector<std::pair<std::string, std::string>> bearerHeader{
      {"Authorization", "Bearer " + config.apiKey}};

  const IrHttpRequest resolveRequest{
      .method = IrHttpMethod::Post,
      .url = *origin + "/api/v1/games/" + game + "/charts/resolve",
      .headers = {{"Authorization", "Bearer " + config.apiKey},
                  {"Content-Type", "application/json"}},
      .body =
          "{\"identifier\":\"" + *sha256 + "\",\"matchType\":\"bmsChartHash\"}",
      .maximumResponseBytes = kMaximumTachiResponseBytes,
      .followRedirects = false,
  };
  const IrHttpResponse resolveResponse =
      http.perform(resolveRequest, stopToken);
  if (const auto failure =
          rankingHttpFailure(resolveResponse, config.apiKey, true)) {
    return *failure;
  }
  const auto resolved =
      parseChartResolveResponse(resolveResponse.body, normalizedQuery);
  if (resolved.status != ChartRankingStatus::Succeeded || !resolved.chartId) {
    return rankingParseFailure(resolved.status, resolved.diagnostic,
                               config.apiKey);
  }

  if (stopToken.stop_requested()) {
    return rankingFailure(ChartRankingStatus::Cancelled,
                          "Tachi ranking request was cancelled");
  }
  const IrHttpRequest identityRequest{
      .method = IrHttpMethod::Get,
      .url = *origin + "/api/v1/status",
      .headers = bearerHeader,
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

  IrChartRanking ranking{.providerId = std::string(kProviderId),
                         .chart = normalizedQuery};
  std::optional<int> expectedEntries;
  std::set<std::int64_t> seenUsers;
  int startRanking = 1;
  while (true) {
    if (stopToken.stop_requested()) {
      return rankingFailure(ChartRankingStatus::Cancelled,
                            "Tachi ranking request was cancelled");
    }
    const IrHttpRequest pageRequest{
        .method = IrHttpMethod::Get,
        .url = *origin + "/api/v1/games/" + game + "/charts/" +
               encodePathSegment(*resolved.chartId) +
               "/pbs?startRanking=" + std::to_string(startRanking),
        .headers = bearerHeader,
        .maximumResponseBytes = kMaximumRankingResponseBytes,
        .followRedirects = false,
    };
    const IrHttpResponse pageResponse = http.perform(pageRequest, stopToken);
    if (const auto failure =
            rankingHttpFailure(pageResponse, config.apiKey, true)) {
      return *failure;
    }
    auto page = parseRankingPageResponse(pageResponse.body, normalizedQuery,
                                         *resolved.chartId, *identity.userId);
    if (page.status != ChartRankingStatus::Succeeded || !page.page) {
      return rankingParseFailure(page.status, page.diagnostic, config.apiKey);
    }

    if (page.page->entries.empty()) {
      if (ranking.entries.empty() && !expectedEntries) {
        return {.status = ChartRankingStatus::Succeeded,
                .ranking = std::move(ranking)};
      }
      return rankingFailure(ChartRankingStatus::MalformedResponse,
                            "Tachi ranking pagination ended early");
    }
    if (!expectedEntries) {
      expectedEntries = page.page->outOf;
      ranking.entries.reserve(static_cast<std::size_t>(*expectedEntries));
    } else if (*expectedEntries != page.page->outOf) {
      return rankingFailure(ChartRankingStatus::MalformedResponse,
                            "Tachi ranking changed during pagination");
    }
    if (page.page->entries.front().rank < startRanking ||
        (!ranking.entries.empty() &&
         page.page->entries.front().rank <= ranking.entries.back().rank)) {
      return rankingFailure(ChartRankingStatus::MalformedResponse,
                            "Tachi ranking pagination order is invalid");
    }
    for (const std::int64_t userId : page.page->userIds) {
      if (!seenUsers.insert(userId).second) {
        return rankingFailure(ChartRankingStatus::MalformedResponse,
                              "Tachi ranking pagination duplicated a user");
      }
    }

    const int lastRank = page.page->entries.back().rank;
    ranking.entries.insert(ranking.entries.end(),
                           std::make_move_iterator(page.page->entries.begin()),
                           std::make_move_iterator(page.page->entries.end()));
    if (ranking.entries.size() > static_cast<std::size_t>(*expectedEntries)) {
      return rankingFailure(ChartRankingStatus::MalformedResponse,
                            "Tachi ranking returned too many entries");
    }
    if (ranking.entries.size() == static_cast<std::size_t>(*expectedEntries)) {
      return {.status = ChartRankingStatus::Succeeded,
              .ranking = std::move(ranking)};
    }
    if (lastRank >= *expectedEntries) {
      return rankingFailure(ChartRankingStatus::MalformedResponse,
                            "Tachi ranking pagination is incomplete");
    }
    startRanking = lastRank + 1;
  }
}

} // namespace ir::tachi
