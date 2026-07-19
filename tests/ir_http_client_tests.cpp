#include "../src/ir/IrHttpClient.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using namespace ir;
using namespace ir::http_testing;

IrHttpRequest request() {
  return {
      .method = IrHttpMethod::Post,
      .url = "https://example.invalid/api/import",
      .headers = {{"Content-Type", "application/json"}},
      .body = R"({"score":123})",
      .maximumResponseBytes = 1024,
      .connectTimeout = std::chrono::seconds(4),
      .totalTimeout = std::chrono::seconds(9),
      .followRedirects = false,
  };
}

void testContractDefaults() {
  const IrHttpRequest value;
  assert(value.method == IrHttpMethod::Get);
  assert(value.maximumResponseBytes == 1024 * 1024);
  assert(value.connectTimeout == std::chrono::seconds(10));
  assert(value.totalTimeout == std::chrono::seconds(25));
  assert(!value.followRedirects);
}

void testInvalidRequestsNeverReachTransport() {
  std::size_t calls = 0;
  Transport transport = [&](const IrHttpRequest &, std::stop_token) {
    ++calls;
    return TransportResponse{.statusCode = 200, .bodyChunks = {"ok"}};
  };

  auto invalid = request();
  invalid.url = "file:///private/result.json";
  auto result = PerformWithTransport(invalid, {}, transport);
  assert(result.transportError == IrTransportError::Other);
  assert(calls == 0);

  invalid = request();
  invalid.headers.push_back({"Unsafe\r\nHeader", "value"});
  result = PerformWithTransport(invalid, {}, transport);
  assert(result.transportError == IrTransportError::Other);
  assert(calls == 0);

  invalid = request();
  invalid.maximumResponseBytes = 0;
  result = PerformWithTransport(invalid, {}, transport);
  assert(result.transportError == IrTransportError::Other);
  assert(calls == 0);

  invalid = request();
  invalid.connectTimeout = std::chrono::seconds(10);
  invalid.totalTimeout = std::chrono::seconds(5);
  result = PerformWithTransport(invalid, {}, transport);
  assert(result.transportError == IrTransportError::Other);
  assert(calls == 0);

  invalid = request();
  invalid.method = IrHttpMethod::Get;
  result = PerformWithTransport(invalid, {}, transport);
  assert(result.transportError == IrTransportError::Other);
  assert(calls == 0);
}

void testSafeHeaderLookupIsCaseInsensitive() {
  Transport transport = [](const IrHttpRequest &, std::stop_token) {
    return TransportResponse{
        .statusCode = 429,
        .headers = {{"content-type", "application/json"},
                    {"rEtRy-AfTeR", " 120 \t"}},
        .bodyChunks = {R"({"error":"busy"})"},
    };
  };
  const auto result = PerformWithTransport(request(), {}, transport);
  assert(result.transportError == IrTransportError::None);
  assert(result.statusCode == 429);
  assert(result.retryAfter == "120");

  transport = [](const IrHttpRequest &, std::stop_token) {
    return TransportResponse{
        .statusCode = 429,
        .headers = {{"Retry-After", "1\r\nX-Injected: true"}},
    };
  };
  assert(!PerformWithTransport(request(), {}, transport).retryAfter);
}

void testResponseBodyCapIsEnforcedAcrossChunks() {
  auto capped = request();
  capped.maximumResponseBytes = 5;
  Transport exact = [](const IrHttpRequest &, std::stop_token) {
    return TransportResponse{.statusCode = 200,
                             .bodyChunks = {"12", "345"}};
  };
  const auto accepted = PerformWithTransport(capped, {}, exact);
  assert(accepted.transportError == IrTransportError::None);
  assert(accepted.body == "12345");

  Transport oversized = [](const IrHttpRequest &, std::stop_token) {
    return TransportResponse{.statusCode = 200,
                             .bodyChunks = {"12", "345", "6"}};
  };
  const auto rejected = PerformWithTransport(capped, {}, oversized);
  assert(rejected.transportError == IrTransportError::ResponseTooLarge);
  assert(rejected.statusCode == 0);
  assert(rejected.body.empty());
}

void testFullUserScoreResponseLimitReachesTransport() {
  auto fullSnapshot = request();
  fullSnapshot.maximumResponseBytes = kMaximumIrHttpResponseBytes;
  std::size_t calls = 0;
  Transport transport = [&](const IrHttpRequest &, std::stop_token) {
    ++calls;
    return TransportResponse{.statusCode = 200, .bodyChunks = {"ok"}};
  };

  const auto accepted = PerformWithTransport(fullSnapshot, {}, transport);
  assert(accepted.transportError == IrTransportError::None);
  assert(accepted.statusCode == 200);
  assert(accepted.body == "ok");
  assert(calls == 1);

  fullSnapshot.maximumResponseBytes += 1;
  const auto rejected = PerformWithTransport(fullSnapshot, {}, transport);
  assert(rejected.transportError == IrTransportError::Other);
  assert(rejected.diagnostic ==
         "IR HTTP request or response size limit is invalid");
  assert(calls == 1);
}

void testCancellationMapsWithoutLeakingIntoTransportErrors() {
  std::size_t calls = 0;
  Transport transport = [&](const IrHttpRequest &, std::stop_token) {
    ++calls;
    return TransportResponse{.transportError = IrTransportError::Other,
                             .diagnostic = "aborted"};
  };
  std::stop_source stopped;
  stopped.request_stop();
  const auto before =
      PerformWithTransport(request(), stopped.get_token(), transport);
  assert(before.transportError == IrTransportError::Cancelled);
  assert(calls == 0);

  std::stop_source during;
  transport = [&](const IrHttpRequest &, std::stop_token) {
    ++calls;
    during.request_stop();
    return TransportResponse{.transportError = IrTransportError::Other,
                             .diagnostic = "callback aborted"};
  };
  const auto after =
      PerformWithTransport(request(), during.get_token(), transport);
  assert(after.transportError == IrTransportError::Cancelled);
  assert(calls == 1);
}

void testAuthenticatedRedirectsAreRejectedWithoutSecretDiagnostics() {
  constexpr std::string_view secret = "never-echo-this-key";
  auto redirected = request();
  redirected.followRedirects = true;
  redirected.headers.push_back(
      {"Authorization", "Bearer " + std::string(secret)});
  std::size_t calls = 0;
  Transport transport = [&](const IrHttpRequest &, std::stop_token) {
    ++calls;
    return TransportResponse{.statusCode = 200};
  };
  const auto result = PerformWithTransport(redirected, {}, transport);
  assert(result.transportError == IrTransportError::Other);
  assert(calls == 0);
  assert(result.diagnostic.find(secret) == std::string::npos);
  assert(result.body.find(secret) == std::string::npos);
}

void testHttpErrorsRemainHttpResponses() {
  Transport transport = [](const IrHttpRequest &, std::stop_token) {
    return TransportResponse{.statusCode = 401,
                             .bodyChunks = {R"({"error":"unauthorized"})"}};
  };
  const auto result = PerformWithTransport(request(), {}, transport);
  assert(result.transportError == IrTransportError::None);
  assert(result.statusCode == 401);
  assert(!result.body.empty());
}

} // namespace

int main() {
  testContractDefaults();
  testInvalidRequestsNeverReachTransport();
  testSafeHeaderLookupIsCaseInsensitive();
  testResponseBodyCapIsEnforcedAcrossChunks();
  testFullUserScoreResponseLimitReachesTransport();
  testCancellationMapsWithoutLeakingIntoTransportErrors();
  testAuthenticatedRedirectsAreRejectedWithoutSecretDiagnostics();
  testHttpErrorsRemainHttpResponses();
  std::cout << "IR HTTP client tests passed\n";
  return 0;
}
