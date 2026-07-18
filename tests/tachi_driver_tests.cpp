#include "ir/tachi/TachiDriver.h"

#include "ir/IrHttpClient.h"

#include <chrono>
#include <deque>
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
    return response;
  }

  std::deque<ir::IrHttpResponse> responses;
  std::vector<ir::IrHttpRequest> requests;
};

ir::IrOutboxEntry pendingEntry(bool userIntent = false) {
  return {
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

} // namespace

int main() {
  testCapabilitiesAndDraftDelegation();
  testImmediateSubmissionRequestAndAcceptedResponse();
  testAutomaticSubmissionOmitsUserIntent();
  testImmediateWarningsAndRejection();
  testMalformedAndBoundedDiagnostics();
  testDeferredAcceptanceAndValidation();
  testPollUsesPersistedOriginAndCurrentKey();
  testCompletedPollsUseImportParser();
  testAwaitingSubmitNeverPostsAgain();
  testHttpAndTransportClassification();
  testInvalidRuntimeConfigurationNeverSends();

  if (failures != 0) {
    std::cerr << failures << " Tachi driver test(s) failed\n";
    return 1;
  }
  std::cout << "Tachi driver tests passed\n";
  return 0;
}
