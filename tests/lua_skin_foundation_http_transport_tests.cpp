#include "skin/beatoraja/LuaSkinFoundationHttpTransportTest.h"

#include <cstddef>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

} // namespace

int main() {
  const skin::LuaSkinHttpLimits limits{.maximumBytes = 4};
  const std::vector<std::byte> oversized(1024 * 1024, std::byte{'a'});
  const auto result = skin::probeLuaSkinFoundationAppend(limits, oversized);
  const std::size_t hardEncodedCeiling =
      limits.maximumBytes;
  expect(!result.continued && result.tooLarge,
         "an oversized single Foundation delegate chunk cancels immediately");
  expect(result.storedBytes <= hardEncodedCeiling,
         "Foundation scans before append and retains only a bounded prefix");
  const std::vector<std::byte> exact(4, std::byte{'\n'});
  const auto atLimit = skin::probeLuaSkinFoundationAppend(limits, exact);
  expect(atLimit.continued && !atLimit.tooLarge && atLimit.storedBytes == 4, "Foundation accepts exact byte capacity including newlines");
  const auto pinned = skin::probeLuaSkinFoundationPinnedRequest("https://example.com:8443/path?q=1", "8.8.8.8");
  expect(!pinned.failure && pinned.url == "https://8.8.8.8:8443/path?q=1", "Foundation connects to validated numeric peer without resolving hostname again");
  expect(pinned.host == "example.com:8443", "Foundation preserves original Host header");
  expect(pinned.redirectsDisabled, "pinned streams disable automatic redirects");
  const auto ipv6 = skin::probeLuaSkinFoundationPinnedRequest("http://example.com/path", "2001:4860:4860::8888");
  expect(!ipv6.failure && ipv6.url == "http://[2001:4860:4860::8888]/path", "Foundation pins IPv6 with valid URL brackets");
  for (auto scenario : {skin::LuaSkinFoundationDecisionTest::Read,
                        skin::LuaSkinFoundationDecisionTest::Disconnect,
                        skin::LuaSkinFoundationDecisionTest::Error,
                        skin::LuaSkinFoundationDecisionTest::DisconnectBeforeResponse,
                        skin::LuaSkinFoundationDecisionTest::CloseBeforeRead}) {
    const auto decision = skin::probeLuaSkinFoundationResponseDecision(scenario);
    const bool read = scenario == skin::LuaSkinFoundationDecisionTest::Read;
    const bool closed = scenario == skin::LuaSkinFoundationDecisionTest::DisconnectBeforeResponse;
    expect(decision.beforeDecision == (closed ? 1 : 0), "headers retain body decision until read or cancellation");
    expect(decision.allowed == (read ? 1 : 0) && decision.cancelled == (read ? 0 : 1),
           "read, error, disconnect and late-response paths consume the completion handler exactly once");
  }
  if (failures != 0) {
    std::cerr << failures << " Foundation HTTP transport test(s) failed\n";
    return 1;
  }
  std::cout << "Foundation HTTP transport tests passed\n";
  return 0;
}
