#pragma once

#include "LuaSkinHttpClient.h"

#include <cstddef>
#include <span>

namespace skin {

struct LuaSkinFoundationAppendProbe {
  std::size_t storedBytes = 0;
  bool continued = false;
  bool tooLarge = false;
};

[[nodiscard]] LuaSkinFoundationAppendProbe
probeLuaSkinFoundationAppend(LuaSkinHttpLimits limits,
                             std::span<const std::byte> chunk);

} // namespace skin

namespace skin {
struct LuaSkinFoundationPinnedRequestProbe {
  std::string url;
  std::string host;
  bool redirectsDisabled = false;
  std::optional<std::string> failure;
};
[[nodiscard]] LuaSkinFoundationPinnedRequestProbe
probeLuaSkinFoundationPinnedRequest(std::string_view url, std::string_view address);
} // namespace skin

#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
#include <functional>
namespace skin {
enum class LuaSkinFoundationDecisionTest { Read, Disconnect, Error, DisconnectBeforeResponse, CloseBeforeRead };
struct LuaSkinFoundationDecisionProbe {
  int beforeDecision = 0;
  int allowed = 0;
  int cancelled = 0;
};
[[nodiscard]] LuaSkinFoundationDecisionProbe
probeLuaSkinFoundationResponseDecision(LuaSkinFoundationDecisionTest scenario);

// Inject the post-validation address list, retaining real native streams. This
// seam exists only in test builds; production always resolves and validates DNS.
struct LuaSkinFoundationCandidateTestOptions {
  std::vector<std::string> candidates;
  std::function<std::chrono::steady_clock::time_point()> now;
  std::function<void(std::size_t)> beforeAttempt;
};
[[nodiscard]] std::unique_ptr<LuaSkinHttpConnection>
createLuaSkinFoundationCandidateTestConnection(std::string url,
    LuaSkinHttpLimits limits, LuaSkinFoundationCandidateTestOptions options);
}
#endif
