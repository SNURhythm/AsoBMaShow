#include "TachiDriver.h"

#include "TachiBatchManual.h"
#include "TachiRankingParser.h"
#include "TachiResponseParser.h"
#include "../IrHttpClient.h"
#include "../IrProfileSettings.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
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
  if (entry.state == IrOutboxState::AwaitingRemoteResult) {
    return poll(entry, config, http, stopToken);
  }
  if (stopToken.stop_requested()) {
    return cancelled();
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

  const IrHttpRequest request{
      .method = IrHttpMethod::Get,
      .url = *origin + "/ir/beatoraja/charts/" + *sha256 + "/scores",
      .headers = {{"Authorization", "Bearer " + config.apiKey}},
      .maximumResponseBytes = kMaximumRankingResponseBytes,
      .followRedirects = false,
  };
  const IrHttpResponse response = http.perform(request, stopToken);
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
                          config.apiKey);
  }
  if (response.statusCode == 404) {
    return {.status = ChartRankingStatus::ChartNotFound};
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

  IrChartQuery normalizedQuery = query;
  normalizedQuery.chartSha256 = *sha256;
  ChartRankingOutcome outcome =
      parseRankingResponse(response.body, normalizedQuery);
  outcome.diagnostic = redact(std::move(outcome.diagnostic), config.apiKey);
  return outcome;
}

} // namespace ir::tachi
