#include "ir/tachi/TachiDriver.h"
#include "ir/tachi/TachiResponseParser.h"

#include "ir/tachi/BokutachiCacheStore.h"

#include "FileChecksum.h"
#include "ir/IrHttpClient.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class FakeHttpClient final : public ir::IrHttpClient {
public:
  ir::IrHttpResponse perform(const ir::IrHttpRequest &request,
                             std::stop_token) noexcept override {
    requests.push_back(request);
    if (responses.empty()) {
      return {.transportError = ir::IrTransportError::Other,
              .diagnostic = "unexpected fake HTTP request"};
    }
    auto response = std::move(responses.front());
    responses.pop_front();
    if (afterResponse) {
      afterResponse(requests.size());
    }
    return response;
  }

  std::deque<ir::IrHttpResponse> responses;
  std::vector<ir::IrHttpRequest> requests;
  std::function<void(std::size_t)> afterResponse;
};

class CacheTempDirectory {
public:
  CacheTempDirectory() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-tachi-driver-cache-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~CacheTempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] std::filesystem::path cachePath() const {
    return path_ / "bokutachi-cache.json";
  }

private:
  std::filesystem::path path_;
};

ir::IrOutboxEntry pendingEntry(bool userIntent = false) {
  ir::IrOutboxEntry entry{
      .id = 1,
      .providerId = "tachi",
      .attemptId = "123e4567-e89b-42d3-a456-426614174000",
      .chartMd5 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      .chartSha256 =
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      .payloadJson =
          R"({"meta":{"game":"bms","playtype":"7K","service":"AsoBMaShow"},"scores":[{"score":123}]})",
      .state = ir::IrOutboxState::Pending,
      .localResultReady = true,
      .nextRequestUserIntent = userIntent,
      .createdAtUnixMillis = 1700000000000LL,
      .updatedAtUnixMillis = 1700000000000LL,
  };
  const std::string proofInput =
      "tachi-lr2-proof-v1\n3:lr2\n3\n" +
      std::to_string(entry.attemptId.size()) + ":" + entry.attemptId + "\n" +
      std::to_string(entry.chartSha256.size()) + ":" + entry.chartSha256 +
      "\n" + std::to_string(entry.payloadJson.size()) + ":" + entry.payloadJson;
  entry.rulesetProof = {
      .rulesetId = "lr2",
      .rulesetRevision = 3,
      .validationFingerprint = file_checksum::sha256(proofInput),
  };
  return entry;
}

ir::IrOutboxEntry awaitingEntry() {
  auto entry = pendingEntry();
  entry.state = ir::IrOutboxState::AwaitingRemoteResult;
  entry.nextRequestUserIntent = false;
  entry.remoteJobId = "0123456789abcdefabcd";
  entry.remoteOrigin = "https://original.example.test";
  return entry;
}

ir::IrProviderRuntimeConfig runtimeConfig() {
  return {.profileId = "profile-a",
          .serverOrigin = "HTTPS://BOKU.TACHI.AC:443/",
          .apiKey = "fresh-api-key"};
}

std::string immediate(std::string_view scoreIds,
                      std::string_view errors = "[]") {
  return std::string(
             R"({"success":true,"description":"Import successful.","body":{"scoreIDs":)") +
         std::string(scoreIds) + R"(,"errors":)" + std::string(errors) +
         R"(}})";
}

std::string deferred(std::string_view bodyFields) {
  return std::string(
             R"({"success":true,"description":"Import queued.","body":{)") +
         std::string(bodyFields) + R"(}})";
}

std::string pollCompleted(std::string_view scoreIds,
                          std::string_view errors = "[]") {
  return std::string(
             R"({"success":true,"description":"Import was completed!","body":{"importStatus":"completed","import":{"scoreIDs":)") +
         std::string(scoreIds) + R"(,"errors":)" + std::string(errors) +
         R"(}}})";
}

bool hasHeader(const ir::IrHttpRequest &request, std::string_view name,
               std::string_view value) {
  for (const auto &[actualName, actualValue] : request.headers) {
    if (actualName == name && actualValue == value) {
      return true;
    }
  }
  return false;
}

bool hasHeaderNamed(const ir::IrHttpRequest &request, std::string_view name) {
  for (const auto &[actualName, value] : request.headers) {
    (void)value;
    if (actualName == name) {
      return true;
    }
  }
  return false;
}

void testCapabilitiesAndDraftDelegation() {
  const ir::tachi::TachiDriver driver;
  expect(driver.providerId() == "tachi", "driver has stable provider ID");
  expect(driver.capabilities() ==
             ir::IrDriverCapabilities{.readOnly = false,
                                      .chartRankings = true,
                                      .scoreSubmission = true,
                                      .deferredSubmission = true},
         "driver declares Bokutachi capabilities");

  ir::IrSubmission unsupported;
  unsupported.keyMode = 5;
  const auto draft = driver.buildDraft(unsupported);
  expect(draft.status == ir::BuildDraftStatus::Unsupported,
         "driver delegates draft construction to Batch Manual mapping");
}

void testImmediateSubmissionRequestAndAcceptedResponse() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back(
      {.statusCode = 200, .body = immediate(R"(["score-1"])")});

  const auto result =
      driver.submit(pendingEntry(true), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::Succeeded,
         "one accepted score succeeds");
  expect(http.requests.size() == 1, "submission performs one request");
  if (http.requests.empty()) {
    return;
  }
  const auto &request = http.requests.front();
  expect(request.method == ir::IrHttpMethod::Post, "submission uses POST");
  expect(request.url == "https://boku.tachi.ac/ir/direct-manual/import",
         "submission normalizes origin and uses exact path");
  expect(request.headers ==
             std::vector<std::pair<std::string, std::string>>{
                 {"Authorization", "Bearer fresh-api-key"},
                 {"Content-Type", "application/json"},
                 {"X-User-Intent", "true"}},
         "manual submission uses exact headers and current bearer");
  expect(request.body == pendingEntry(true).payloadJson,
         "submission sends the immutable outbox payload");
  expect(request.maximumResponseBytes == 1024 * 1024,
         "submission caps the response at one MiB");
  expect(!request.followRedirects,
         "authenticated submission does not follow redirects");
}

void testAutomaticSubmissionOmitsUserIntent() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back(
      {.statusCode = 200, .body = immediate(R"(["score-1"])")});
  const auto result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::Succeeded,
         "automatic submission succeeds");
  expect(http.requests.size() == 1 &&
             !hasHeaderNamed(http.requests.front(), "X-User-Intent"),
         "automatic submission omits X-User-Intent");
}

void testBlocksLegacyAndMismatchedRulesetProofsBeforeHttp() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;

  auto entry = pendingEntry();
  entry.rulesetProof = {.rulesetId = "legacy-unknown"};
  auto result = driver.submit(entry, runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure,
         "legacy queued row is rejected");
  expect(result.code == "ruleset_proof_mismatch",
         "legacy queued row has a stable proof code");
  expect(http.requests.empty(), "legacy proof performs no HTTP request");

  entry = pendingEntry();
  entry.payloadJson.push_back(' ');
  result = driver.submit(entry, runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure,
         "payload changed after validation is blocked");
  expect(result.code == "ruleset_proof_mismatch",
         "proof mismatch has a stable integrity code");
  expect(http.requests.empty(), "proof mismatch performs no HTTP request");

  entry = pendingEntry();
  entry.providerId = "other";
  result = driver.submit(entry, runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure &&
             result.code == "ruleset_proof_mismatch",
         "provider mismatch is rejected as a proof mismatch");
  expect(http.requests.empty(), "provider mismatch performs no HTTP request");

  entry = awaitingEntry();
  entry.payloadJson.push_back(' ');
  result = driver.poll(entry, runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure &&
             result.code == "ruleset_proof_mismatch",
         "polling also validates the frozen proof");
  expect(http.requests.empty(), "poll proof mismatch performs no HTTP request");
}

void testImmediateWarningsAndRejection() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back(
      {.statusCode = 200,
       .body = immediate(
           R"(["score-1"])",
           R"([{"type":"InvalidDatapoint","message":"gauge was adjusted"}])")});
  auto result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::Succeeded,
         "accepted score with warning still succeeds");
  expect(result.diagnostic.find("gauge was adjusted") != std::string::npos,
         "accepted warning is retained as a diagnostic");

  http.responses.push_back(
      {.statusCode = 200,
       .body = immediate(
           "[]",
           R"([{"type":"ConverterError","message":"chart was not found"}])")});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure,
         "converter rejection without accepted score is permanent");
  expect(result.code == "import_rejected",
         "converter rejection has a stable error code");
  expect(result.diagnostic.find("chart was not found") != std::string::npos,
         "converter diagnostic is retained");
}

void testImmediateImportPreservesIdentity() {
  const auto outcome = ir::tachi::parseImmediateImportResponse(
      R"({"success":true,"body":{"userID":42,"scoreIDs":["Tscore"],"errors":[]}})");
  expect(outcome.status == ir::DeliveryStatus::Succeeded,
         "identity import succeeds");
  expect(outcome.remoteUserId == 42, "user identity is retained");
  expect(outcome.remoteScoreId == "Tscore", "score identity is retained");
}

void testDuplicateImportIsIdempotentSuccess() {
  const auto outcome = ir::tachi::parseImmediateImportResponse(
      R"({"success":true,"body":{"userID":42,"scoreIDs":[],"errors":[]}})");
  expect(outcome.status == ir::DeliveryStatus::Succeeded,
         "duplicate import is successful");
  expect(outcome.code == "already_exists", "duplicate has stable code");
  expect(!outcome.remoteScoreId, "duplicate has no fabricated score ID");
}

void testEmptyRejectedImportRemainsPermanentFailure() {
  const auto outcome = ir::tachi::parseImmediateImportResponse(
      R"({"success":true,"body":{"userID":42,"scoreIDs":[],"errors":[{"type":"ConverterError","message":"chart was not found"}]}})");
  expect(outcome.status == ir::DeliveryStatus::PermanentFailure,
         "empty rejected import remains a permanent failure");
  expect(outcome.code == "import_rejected",
         "empty rejected import retains its rejection code");
}

void testInvalidImportUserIdentityIsIgnored() {
  for (const std::string_view userId : {
           std::string_view{"-1"}, std::string_view{"0"},
           std::string_view{"\"invalid\""},
           std::string_view{"9223372036854775808"},
       }) {
    const auto outcome = ir::tachi::parseImmediateImportResponse(
        std::string(R"({"success":true,"body":{"userID":)") +
        std::string(userId) + R"(,"scoreIDs":["Tscore"],"errors":[]}})");
    expect(outcome.status == ir::DeliveryStatus::Succeeded,
           "invalid user identity does not reject the import");
    expect(!outcome.remoteUserId,
           "invalid or negative user identity is not retained");
  }
}

void testOversizedScoreIdentityIsMalformed() {
  const auto outcome = ir::tachi::parseImmediateImportResponse(
      std::string(R"({"success":true,"body":{"userID":42,"scoreIDs":[")") +
      std::string(ir::kMaximumIrRemoteValueBytes + 1, 's') +
      R"("],"errors":[]}})");
  expect(outcome.status == ir::DeliveryStatus::PermanentFailure,
         "oversized score identity is rejected");
  expect(outcome.code == "malformed_response",
         "oversized score identity remains malformed");
}

void testMultipleScoreIdentitiesAreMalformed() {
  const auto outcome = ir::tachi::parseImmediateImportResponse(
      R"({"success":true,"body":{"userID":42,"scoreIDs":["score-1","score-2"],"errors":[]}})");
  expect(outcome.status == ir::DeliveryStatus::PermanentFailure,
         "multiple score identities violate the single-score contract");
  expect(outcome.code == "malformed_response",
         "multiple score identities are malformed");
}

void testMalformedAndBoundedDiagnostics() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back({.statusCode = 200, .body = "not-json"});
  auto result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure,
         "malformed successful response is permanent");
  expect(result.code == "malformed_response",
         "malformed response has a stable error code");

  const std::string unsafeMessage = std::string(900, 'x') + R"(\u0001tail)";
  http.responses.push_back(
      {.statusCode = 200,
       .body = immediate(
           "[]", std::string(R"([{"type":"ConverterError","message":")") +
                     unsafeMessage + R"("}])")});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure,
         "oversized converter diagnostic remains a rejection");
  expect(result.code == "import_rejected",
         "oversized converter diagnostic was parsed as an import rejection");
  expect(result.diagnostic.size() <= ir::kMaximumDiagnosticBytes,
         "converter diagnostic is bounded");
  expect(result.diagnostic.find('\x01') == std::string::npos,
         "converter diagnostic removes unsafe control bytes");
}

void testDeferredAcceptanceAndValidation() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back(
      {.statusCode = 202,
       .body = deferred(
           R"("url":"https://evil.example/poll","importID":"0123456789abcdefabcd")")});
  auto result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::Deferred,
         "valid queued import is deferred");
  expect(result.remoteJobId == "0123456789abcdefabcd",
         "queued import persists validated import ID");
  expect(result.remoteOrigin == "https://boku.tachi.ac",
         "queued import persists normalized request origin");
  expect(result.remoteOrigin != "https://evil.example",
         "response-provided polling URL is ignored");

  http.responses.push_back(
      {.statusCode = 202,
       .body =
           R"({"url":"https://evil.example/poll","importID":"raw-import-123"})"});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::Deferred,
         "documented raw queued import is deferred");
  expect(result.remoteJobId == "raw-import-123",
         "raw queued import persists its validated import ID");
  expect(result.remoteOrigin == "https://boku.tachi.ac",
         "raw queued import keeps the normalized request origin");

  for (const std::string body : {
           deferred(R"("url":"https://evil.example/poll")"),
           deferred(R"("importID":"bad/id")"),
           deferred(R"("importID":"é")"),
           deferred(R"("importID":"")"),
           deferred(std::string(R"("importID":")") + std::string(129, 'a') +
                    R"(")"),
       }) {
    http.responses.push_back({.statusCode = 202, .body = body});
    result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
    expect(result.status == ir::DeliveryStatus::PermanentFailure,
           "missing, unsafe, or oversized import ID is permanent");
    expect(result.code == "invalid_import_id",
           "invalid 202 data has a stable error code");
  }
}

void testPollUsesPersistedOriginAndCurrentKey() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back(
      {.statusCode = 200,
       .body =
           R"({"success":true,"description":"Import is ongoing.","body":{"importStatus":"ongoing","progress":{"description":"Importing scores."}}})"});
  auto config = runtimeConfig();
  config.serverOrigin = "https://changed.example.test";
  config.apiKey = "replacement-key";

  auto result = driver.poll(awaitingEntry(), config, http, {});
  expect(result.status == ir::DeliveryStatus::Ongoing,
         "ongoing poll remains deferred");
  expect(http.requests.size() == 1, "poll performs one request");
  if (http.requests.empty()) {
    return;
  }
  const auto &request = http.requests.front();
  expect(request.method == ir::IrHttpMethod::Get, "poll uses GET");
  expect(request.url == "https://original.example.test/api/v1/imports/"
                        "0123456789abcdefabcd/poll-status",
         "poll uses persisted request origin and exact path");
  expect(request.headers ==
             std::vector<std::pair<std::string, std::string>>{
                 {"Authorization", "Bearer replacement-key"}},
         "poll sends only the current bearer header");
  expect(request.body.empty(), "poll has no request body");
  expect(!request.followRedirects,
         "authenticated poll does not follow redirects");
}

void testCompletedPollsUseImportParser() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back(
      {.statusCode = 200, .body = pollCompleted(R"(["accepted-score"])")});
  auto result = driver.poll(awaitingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::Succeeded,
         "completed poll with accepted score succeeds");

  http.responses.push_back(
      {.statusCode = 200,
       .body = pollCompleted(
           "[]", R"([{"type":"ConverterError","message":"invalid chart"}])")});
  result = driver.poll(awaitingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure,
         "completed rejected import is permanent");
  expect(result.diagnostic.find("invalid chart") != std::string::npos,
         "completed rejection retains the converter diagnostic");
}

void testAwaitingSubmitNeverPostsAgain() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back(
      {.statusCode = 200,
       .body = R"({"success":true,"body":{"importStatus":"ongoing"}})"});
  const auto result = driver.submit(awaitingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::Ongoing,
         "submitting an awaiting row polls it");
  expect(http.requests.size() == 1 &&
             http.requests.front().method == ir::IrHttpMethod::Get,
         "awaiting row never causes a second POST");
}

void testHttpAndTransportClassification() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;

  http.responses.push_back({.statusCode = 302, .body = "redirect"});
  auto result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure,
         "redirect response is permanent");
  expect(result.code == "redirect_rejected",
         "redirect response has a stable code");

  http.responses.push_back(
      {.statusCode = 429, .body = "busy", .retryAfter = "120"});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::TransientFailure,
         "429 is transient");
  expect(result.retryAfterDelay == std::chrono::seconds(120),
         "valid delta Retry-After is parsed");

  http.responses.push_back(
      {.statusCode = 503, .body = "busy", .retryAfter = "tomorrow"});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::TransientFailure &&
             !result.retryAfterDelay,
         "invalid Retry-After is ignored on transient status");

  http.responses.push_back({.statusCode = 401});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::BlockedConfiguration,
         "401 blocks configuration");

  http.responses.push_back({.statusCode = 403});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::BlockedConfiguration,
         "403 blocks configuration");

  http.responses.push_back({.statusCode = 400, .body = "bad request"});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::PermanentFailure,
         "ordinary 4xx is permanent");

  http.responses.push_back(
      {.statusCode = 400,
       .body = R"({"success":false,"description":"bad fresh-api-key token"})"});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(
      result.diagnostic.find("fresh-api-key") == std::string::npos,
      "server diagnostics cannot copy the current API key into outbox state");

  http.responses.push_back({.statusCode = 408, .retryAfter = "3"});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::TransientFailure &&
             result.retryAfterDelay == std::chrono::seconds(3),
         "408 is transient and honors delta Retry-After");

  http.responses.push_back({.transportError = ir::IrTransportError::Offline,
                            .diagnostic = "offline"});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::TransientFailure,
         "transport errors are transient");

  http.responses.push_back({.transportError = ir::IrTransportError::Cancelled,
                            .diagnostic = "cancelled"});
  result = driver.submit(pendingEntry(), runtimeConfig(), http, {});
  expect(result.status == ir::DeliveryStatus::Cancelled,
         "cancelled transport maps separately");
}

void testInvalidRuntimeConfigurationNeverSends() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  auto config = runtimeConfig();
  config.apiKey.clear();
  auto result = driver.submit(pendingEntry(), config, http, {});
  expect(result.status == ir::DeliveryStatus::BlockedConfiguration,
         "missing key blocks submission");
  expect(http.requests.empty(), "missing key never reaches HTTP");

  config = runtimeConfig();
  config.serverOrigin = "https://user@example.test/path";
  result = driver.submit(pendingEntry(), config, http, {});
  expect(result.status == ir::DeliveryStatus::BlockedConfiguration,
         "invalid configured origin blocks submission");
  expect(http.requests.empty(), "invalid origin never reaches HTTP");
}

ir::IrChartQuery rankingQuery() {
  return {
      .keyMode = 7,
      .chartMd5 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      .chartSha256 =
          "ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD",
      .totalNotes = 100,
  };
}

std::string rankingResolveBody(int keyMode = 7) {
  const auto query = rankingQuery();
  return nlohmann::json{{"success", true},
                        {"body",
                         {{"chart",
                           {{"chartID", "chart-id"},
                            {"game", keyMode == 14 ? "bms-14k" : "bms-7k"},
                            {"data",
                             {{"hashSHA256", "abcdefabcdefabcdefabcdefabcdefabc"
                                             "defabcdefabcdefabcdefabcdefabcd"},
                              {"notecount", query.totalNotes}}}}},
                          {"song", {{"id", "song-id"}}}}}}
      .dump();
}

std::string rankingIdentityBody(std::int64_t userId = 42) {
  return nlohmann::json{{"success", true}, {"body", {{"whoami", userId}}}}
      .dump();
}

nlohmann::json rankingPb(int rank, int outOf, std::int64_t userId) {
  return {
      {"composedFrom", nlohmann::json::array(
                           {{{"name", "Best Score"},
                             {"scoreID", "score-" + std::to_string(userId)}}})},
      {"rankingData", {{"rank", rank}, {"outOf", outOf}}},
      {"userID", userId},
      {"chartID", "chart-id"},
      {"game", "bms-7k"},
      {"timeAchieved", 1700000000000LL + rank},
      {"scoreData",
       {{"score", 100},
        {"lamp", "CLEAR"},
        {"enumIndexes", {{"lamp", 4}}},
        {"optional",
         {{"enumIndexes", nlohmann::json::object()},
          {"epg", 50},
          {"lpg", 0},
          {"egr", 0},
          {"lgr", 0},
          {"bp", 2},
          {"maxCombo", 80}}}}},
  };
}

std::string rankingPageBody(const nlohmann::json &pbs,
                            const nlohmann::json &users) {
  return nlohmann::json{{"success", true},
                        {"body", {{"pbs", pbs}, {"users", users}}}}
      .dump();
}

void testRankingRequestAndStatusClassification() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back({.statusCode = 200, .body = rankingResolveBody()});
  http.responses.push_back({.statusCode = 200, .body = rankingIdentityBody()});
  http.responses.push_back({.statusCode = 200,
                            .body = rankingPageBody(nlohmann::json::array(),
                                                    nlohmann::json::array())});
  auto result =
      driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded && result.ranking &&
             result.ranking->entries.empty(),
         "empty remote chart ranking succeeds");
  expect(http.requests.size() == 3,
         "native ranking fetch resolves, authenticates, and reads the first "
         "ranking page");
  if (http.requests.size() == 3) {
    const auto &resolve = http.requests[0];
    expect(resolve.method == ir::IrHttpMethod::Post,
           "ranking chart resolution uses POST");
    expect(resolve.url ==
               "https://boku.tachi.ac/api/v1/games/bms-7k/charts/resolve",
           "ranking fetch uses the native BMS chart resolver");
    expect(resolve.headers ==
               std::vector<std::pair<std::string, std::string>>{
                   {"Content-Type", "application/json"}},
           "native chart resolution is public and sends JSON metadata only");
    expect(
        resolve.body ==
            R"({"identifier":"abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd","matchType":"bmsChartHash"})",
        "native chart resolution sends the canonical hash");
    expect(!resolve.followRedirects,
           "authenticated ranking fetch does not follow redirects");

    const auto &identity = http.requests[1];
    expect(identity.method == ir::IrHttpMethod::Get &&
               identity.url == "https://boku.tachi.ac/api/v1/status",
           "ranking fetch obtains the authenticated numeric user ID");
    expect(identity.headers ==
               std::vector<std::pair<std::string, std::string>>{
                   {"Authorization", "Bearer fresh-api-key"}},
           "only the optional identity lookup receives bearer auth");

    const auto &page = http.requests[2];
    expect(page.method == ir::IrHttpMethod::Get,
           "native ranking page uses GET");
    expect(page.url == "https://boku.tachi.ac/api/v1/games/bms-7k/charts/"
                       "chart-id/pbs?startRanking=1",
           "ranking fetch reads native PB documents");
    expect(page.headers.empty(), "native PB request is public");
    expect(page.maximumResponseBytes == 8 * 1024 * 1024,
           "ranking page response is capped at eight MiB");
  }

  http.responses.push_back({.statusCode = 404});
  result = driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::ChartNotFound,
         "ranking 404 maps to chart not found");

  for (long status : {401L, 403L}) {
    http.responses.push_back({.statusCode = status});
    result =
        driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
    expect(result.status == ir::ChartRankingStatus::AuthenticationRequired,
           "ranking authentication failure requests credentials");
  }

  for (long status : {408L, 429L, 500L, 503L}) {
    http.responses.push_back({.statusCode = status});
    result =
        driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
    expect(result.status == ir::ChartRankingStatus::TransientFailure,
           "retryable ranking HTTP status is transient");
  }

  for (long status : {302L, 400L}) {
    http.responses.push_back({.statusCode = status});
    result =
        driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
    expect(result.status == ir::ChartRankingStatus::MalformedResponse,
           "other ranking HTTP status is malformed and not cacheable");
  }

  http.responses.push_back(
      {.transportError = ir::IrTransportError::ResponseTooLarge});
  result = driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::OversizedResponse,
         "oversized ranking transport maps separately");

  http.responses.push_back({.transportError = ir::IrTransportError::Offline,
                            .diagnostic = "offline"});
  result = driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::TransientFailure,
         "ranking transport failure is transient");

  http.responses.push_back({.transportError = ir::IrTransportError::Other,
                            .diagnostic = "transport echoed fresh-api-key"});
  result = driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.diagnostic.find("fresh-api-key") == std::string::npos,
         "ranking diagnostics cannot retain the current API key");

  http.responses.push_back({.transportError = ir::IrTransportError::Cancelled});
  result = driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Cancelled,
         "cancelled ranking fetch maps separately");
}

void testNativeRankingPagesWithoutRepeatingPreflight() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  http.responses.push_back({.statusCode = 200, .body = rankingResolveBody()});
  http.responses.push_back({.statusCode = 200, .body = rankingIdentityBody(2)});
  http.responses.push_back(
      {.statusCode = 200,
       .body = rankingPageBody(
           nlohmann::json::array(
               {rankingPb(1, 3, 1), rankingPb(1, 3, 2)}),
           nlohmann::json::array({{{"id", 1}, {"username", "Alice"}},
                                  {{"id", 2}, {"username", "Bob"}}}))});

  auto result =
      driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded && result.ranking &&
             result.ranking->entries.size() == 2 &&
             result.ranking->nextPageToken.has_value(),
         "native ranking returns the first page and a continuation cursor");
  expect(result.ranking && result.ranking->entries[1].currentUser,
         "native user ID marks the authenticated ranking entry");
  expect(result.ranking && result.ranking->entries[0].providerEntryId == "1" &&
             result.ranking->entries[1].providerEntryId == "2",
         "native rows carry stable credential-free provider identities");
  expect(http.requests.size() == 3 &&
             http.requests.back().url.ends_with("?startRanking=1"),
         "initial native ranking always starts at rank one");

  const std::string cursor = *result.ranking->nextPageToken;
  http.responses.push_back(
      {.statusCode = 200,
       .body = rankingPageBody(
           nlohmann::json::array({rankingPb(3, 3, 3)}),
           nlohmann::json::array({{{"id", 3}, {"username", "Carol"}}}))});
  result = driver.fetchChartRankingPage(rankingQuery(), cursor,
                                        runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded && result.ranking &&
             result.ranking->entries.size() == 1 &&
             !result.ranking->nextPageToken.has_value(),
         "the final native page clears the continuation cursor");
  expect(http.requests.size() == 4 &&
             http.requests.back().url.ends_with("?startRanking=2"),
         "a continuation performs exactly one ranking-page request");

  const auto requestsBeforeMalformed = http.requests.size();
  result = driver.fetchChartRankingPage(rankingQuery(), "not-a-page-token",
                                        runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::MalformedResponse &&
             http.requests.size() == requestsBeforeMalformed,
         "a malformed continuation cursor fails before HTTP");

  http.responses.push_back(
      {.statusCode = 200,
       .body = rankingPageBody(
           nlohmann::json::array({rankingPb(1, 3, 3)}),
           nlohmann::json::array({{{"id", 3}, {"username", "Carol"}}}))});
  result = driver.fetchChartRankingPage(rankingQuery(), cursor,
                                        runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::MalformedResponse,
         "a regressing native page is rejected");

  http.responses.push_back({.statusCode = 200,
                            .body = rankingPageBody(nlohmann::json::array(),
                                                    nlohmann::json::array())});
  result = driver.fetchChartRankingPage(rankingQuery(), cursor,
                                        runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::MalformedResponse,
         "a rank-only cursor that cannot advance through a tie fails safely");

  http.responses.push_back(
      {.statusCode = 200,
       .body = rankingPageBody(
           nlohmann::json::array({rankingPb(3, 4, 3)}),
           nlohmann::json::array({{{"id", 3}, {"username", "Carol"}}}))});
  result = driver.fetchChartRankingPage(rankingQuery(), cursor,
                                        runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::MalformedResponse,
         "a changed remote outOf cannot be appended to the loaded prefix");
}

void testRankingPrerequisitesPersistAcrossFetches() {
  CacheTempDirectory temp;
  auto cache = std::make_shared<ir::tachi::BokutachiCacheStore>();
  std::string diagnostic;
  expect(cache->activate(temp.cachePath(), diagnostic),
         "ranking cache fixture activates");
  const ir::tachi::TachiDriver driver(cache);
  FakeHttpClient http;
  http.responses.push_back({.statusCode = 200, .body = rankingResolveBody()});
  http.responses.push_back({.statusCode = 200, .body = rankingIdentityBody()});
  http.responses.push_back({.statusCode = 200,
                            .body = rankingPageBody(nlohmann::json::array(),
                                                    nlohmann::json::array())});
  auto result =
      driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded,
         "cold cached ranking fetch succeeds");
  expect(cache->userId("https://boku.tachi.ac") == 42,
         "successful identity lookup populates cache");
  expect(
      cache->chartId(
          "https://boku.tachi.ac", "bms-7k",
          "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd") ==
          "chart-id",
      "successful chart resolution populates cache");

  http.requests.clear();
  http.responses.push_back({.statusCode = 200,
                            .body = rankingPageBody(nlohmann::json::array(),
                                                    nlohmann::json::array())});
  result = driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded &&
             http.requests.size() == 1 &&
             http.requests.front().url.ends_with("/pbs?startRanking=1"),
         "full persistent cache hit performs only the PB request");

  expect(cache->clearUserIds(diagnostic), "partial hit fixture clears user");
  http.requests.clear();
  http.responses.push_back(
      {.statusCode = 200, .body = rankingIdentityBody(43)});
  http.responses.push_back({.statusCode = 200,
                            .body = rankingPageBody(nlohmann::json::array(),
                                                    nlohmann::json::array())});
  result = driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded &&
             http.requests.size() == 2 &&
             http.requests[0].url.ends_with("/api/v1/status") &&
             http.requests[1].url.ends_with("/pbs?startRanking=1"),
         "chart-only cache hit requests only identity and PB data");

  expect(cache->eraseChartId(
             "https://boku.tachi.ac", "bms-7k",
             "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd",
             diagnostic),
         "partial hit fixture clears chart");
  http.requests.clear();
  http.responses.push_back({.statusCode = 200, .body = rankingResolveBody()});
  http.responses.push_back({.statusCode = 200,
                            .body = rankingPageBody(nlohmann::json::array(),
                                                    nlohmann::json::array())});
  result = driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded &&
             http.requests.size() == 2 &&
             http.requests[0].url.ends_with("/charts/resolve") &&
             http.requests[1].url.ends_with("/pbs?startRanking=1"),
         "user-only cache hit requests only chart resolution and PB data");
}

void testStaleCachedChartIsResolvedOnce() {
  CacheTempDirectory temp;
  auto cache = std::make_shared<ir::tachi::BokutachiCacheStore>();
  std::string diagnostic;
  expect(cache->activate(temp.cachePath(), diagnostic) &&
             cache->rememberUserId("https://boku.tachi.ac", 42, diagnostic) &&
             cache->rememberChartId("https://boku.tachi.ac", "bms-7k",
                                    "abcdefabcdefabcdefabcdefabcdefabcdefabcdef"
                                    "abcdefabcdefabcdefabcd",
                                    "stale-chart-id", diagnostic),
         "stale chart fixture populates cache");
  const ir::tachi::TachiDriver driver(cache);
  FakeHttpClient http;
  http.responses.push_back({.statusCode = 404});
  http.responses.push_back({.statusCode = 200, .body = rankingResolveBody()});
  http.responses.push_back({.statusCode = 200,
                            .body = rankingPageBody(nlohmann::json::array(),
                                                    nlohmann::json::array())});
  auto result =
      driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded &&
             http.requests.size() == 3,
         "cached chart 404 resolves and retries exactly once");
  if (http.requests.size() == 3) {
    expect(http.requests[0].url.find("/stale-chart-id/pbs") !=
                   std::string::npos &&
               http.requests[1].url.ends_with("/charts/resolve") &&
               http.requests[2].url.find("/chart-id/pbs") != std::string::npos,
           "stale chart retry uses old PB, resolver, then replacement PB");
  }
  expect(
      cache->chartId(
          "https://boku.tachi.ac", "bms-7k",
          "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd") ==
          "chart-id",
      "successful stale recovery persists replacement chart identity");

  expect(cache->rememberChartId(
             "https://boku.tachi.ac", "bms-7k",
             "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd",
             "stale-again", diagnostic),
         "second stale fixture updates mapping");
  http.requests.clear();
  http.responses.push_back({.statusCode = 404});
  http.responses.push_back({.statusCode = 200, .body = rankingResolveBody()});
  http.responses.push_back({.statusCode = 404});
  result = driver.fetchChartRanking(rankingQuery(), runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::ChartNotFound &&
             http.requests.size() == 3,
         "replacement chart 404 is returned without an unbounded retry");
}

void testCancelledIdentityResponseIsNotCached() {
  CacheTempDirectory temp;
  auto cache = std::make_shared<ir::tachi::BokutachiCacheStore>();
  std::string diagnostic;
  expect(cache->activate(temp.cachePath(), diagnostic),
         "cancelled identity cache fixture activates");
  const ir::tachi::TachiDriver driver(cache);
  FakeHttpClient http;
  std::stop_source cancellation;
  http.responses.push_back({.statusCode = 200, .body = rankingResolveBody()});
  http.responses.push_back({.statusCode = 200, .body = rankingIdentityBody()});
  http.afterResponse = [&](std::size_t requestCount) {
    if (requestCount == 2) {
      cancellation.request_stop();
    }
  };
  const auto result = driver.fetchChartRanking(
      rankingQuery(), runtimeConfig(), http, cancellation.get_token());
  expect(result.status == ir::ChartRankingStatus::Cancelled,
         "identity response cancelled by credential invalidation is discarded");
  expect(!cache->userId("https://boku.tachi.ac"),
         "cancelled identity response cannot repopulate cached user ID");
}

void testAnonymousRankingAndPagination() {
  CacheTempDirectory temp;
  auto cache = std::make_shared<ir::tachi::BokutachiCacheStore>();
  std::string diagnostic;
  expect(cache->activate(temp.cachePath(), diagnostic) &&
             cache->rememberUserId("https://boku.tachi.ac", 42, diagnostic),
         "anonymous ranking fixture persists a prior authenticated user");
  const ir::tachi::TachiDriver driver(cache);
  FakeHttpClient http;
  auto config = runtimeConfig();
  config.apiKey.clear();
  http.responses.push_back({.statusCode = 200, .body = rankingResolveBody()});
  http.responses.push_back(
      {.statusCode = 200,
       .body = rankingPageBody(
           nlohmann::json::array({rankingPb(1, 3, 42), rankingPb(2, 3, 7)}),
           nlohmann::json::array({{{"id", 42}, {"username", "CachedUser"}},
                                  {{"id", 7}, {"username", "OtherUser"}}}))});

  auto result = driver.fetchChartRanking(rankingQuery(), config, http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded && result.ranking &&
             result.ranking->entries.size() == 2 &&
             result.ranking->nextPageToken.has_value(),
         "anonymous ranking fetch returns a paged public leaderboard");
  expect(result.ranking && !result.ranking->entries[0].currentUser &&
             !result.ranking->entries[1].currentUser,
         "anonymous ranking ignores a cached authenticated user identity");
  expect(http.requests.size() == 2 &&
             http.requests[0].url.ends_with("/charts/resolve") &&
             http.requests[0].headers ==
                 std::vector<std::pair<std::string, std::string>>{
                     {"Content-Type", "application/json"}} &&
             http.requests[1].url.ends_with("/pbs?startRanking=1") &&
             http.requests[1].headers.empty(),
         "anonymous ranking skips identity and sends no authorization header");
  const auto cursor = result.ranking
                          ? result.ranking->nextPageToken.value_or("")
                          : std::string{};
  expect(cursor.find(R"("userID":null)") != std::string::npos &&
             cursor.find("fresh-api-key") == std::string::npos,
         "anonymous continuation token carries no identity or credential");

  http.responses.push_back(
      {.statusCode = 200,
       .body = rankingPageBody(
           nlohmann::json::array({rankingPb(3, 3, 9)}),
           nlohmann::json::array({{{"id", 9}, {"username", "LastUser"}}}))});
  result =
      driver.fetchChartRankingPage(rankingQuery(), cursor, config, http, {});
  expect(result.status == ir::ChartRankingStatus::Succeeded && result.ranking &&
             result.ranking->entries.size() == 1 &&
             !result.ranking->entries.front().currentUser &&
             !result.ranking->nextPageToken,
         "anonymous continuation succeeds without current-user highlighting");
  expect(http.requests.size() == 3 && http.requests.back().headers.empty() &&
             http.requests.back().url.ends_with("?startRanking=3"),
         "anonymous continuation performs one public PB request");
}

void testRankingPreflightNeverLeaksCredentials() {
  const ir::tachi::TachiDriver driver;
  FakeHttpClient http;
  auto config = runtimeConfig();
  config.apiKey = "invalid credential";
  auto result = driver.fetchChartRanking(rankingQuery(), config, http, {});
  expect(result.status == ir::ChartRankingStatus::AuthenticationRequired,
         "a configured malformed ranking credential requests authentication");
  expect(http.requests.empty(), "malformed ranking credential never sends");

  config = runtimeConfig();
  config.serverOrigin = "https://example.test/path";
  result = driver.fetchChartRanking(rankingQuery(), config, http, {});
  expect(result.status == ir::ChartRankingStatus::MalformedResponse,
         "invalid ranking origin fails preflight");
  expect(http.requests.empty(), "invalid ranking origin never sends");

  auto unsupported = rankingQuery();
  unsupported.keyMode = 5;
  result = driver.fetchChartRanking(unsupported, runtimeConfig(), http, {});
  expect(result.status == ir::ChartRankingStatus::Unsupported,
         "Tachi ranking reads reject unsupported BMS key modes");
  expect(http.requests.empty(), "unsupported key mode never sends");
}

} // namespace

int main() {
  testCapabilitiesAndDraftDelegation();
  testImmediateSubmissionRequestAndAcceptedResponse();
  testAutomaticSubmissionOmitsUserIntent();
  testBlocksLegacyAndMismatchedRulesetProofsBeforeHttp();
  testImmediateWarningsAndRejection();
  testImmediateImportPreservesIdentity();
  testDuplicateImportIsIdempotentSuccess();
  testEmptyRejectedImportRemainsPermanentFailure();
  testInvalidImportUserIdentityIsIgnored();
  testOversizedScoreIdentityIsMalformed();
  testMultipleScoreIdentitiesAreMalformed();
  testMalformedAndBoundedDiagnostics();
  testDeferredAcceptanceAndValidation();
  testPollUsesPersistedOriginAndCurrentKey();
  testCompletedPollsUseImportParser();
  testAwaitingSubmitNeverPostsAgain();
  testHttpAndTransportClassification();
  testInvalidRuntimeConfigurationNeverSends();
  testRankingRequestAndStatusClassification();
  testNativeRankingPagesWithoutRepeatingPreflight();
  testRankingPrerequisitesPersistAcrossFetches();
  testStaleCachedChartIsResolvedOnce();
  testCancelledIdentityResponseIsNotCached();
  testAnonymousRankingAndPagination();
  testRankingPreflightNeverLeaksCredentials();

  if (failures != 0) {
    std::cerr << failures << " Tachi driver test(s) failed\n";
    return 1;
  }
  std::cout << "Tachi driver tests passed\n";
  return 0;
}
