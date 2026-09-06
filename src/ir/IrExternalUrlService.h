#pragma once

#include "../ThreadCompat.h"
#include "IrDriver.h"
#include "IrSubmissionService.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace ir {

enum class IrExternalUrlTarget { None, Chart, Course };

struct IrExternalUrlRequest {
  IrActiveProfileConfig profile;
  IrExternalUrlTarget target = IrExternalUrlTarget::None;
  IrChartExternalUrlRequest chart;
};

struct IrExternalUrlSnapshot {
  std::uint64_t generation = 0;
  bool finished = false;
  std::optional<std::string> url;
};

template <typename CredentialLookup, typename AccountLookup,
          typename ChartUrlLookup, typename CourseUrlLookup>
[[nodiscard]] inline std::optional<std::string>
resolveFirstEnabledIrExternalUrl(
    const IrExternalUrlRequest &request, std::stop_token stopToken,
    CredentialLookup &&credentialLookup, AccountLookup &&accountLookup,
    ChartUrlLookup &&chartUrlLookup,
    CourseUrlLookup &&courseUrlLookup) noexcept {
  if (stopToken.stop_requested()) {
    return std::nullopt;
  }
  try {
    for (const auto &[providerId, provider] : request.profile.providers) {
      if (stopToken.stop_requested()) {
        return std::nullopt;
      }
      if (!provider.enabled) {
        continue;
      }
      const std::string apiKey =
          credentialLookup(request.profile.profileId, providerId);
      if (apiKey.empty()) {
        continue;
      }
      const IrProviderRuntimeConfig runtime{
          .profileId = request.profile.profileId,
          .serverOrigin = provider.serverOrigin,
          .apiKey = apiKey,
      };
      const auto account =
          accountLookup(providerId, runtime, stopToken);
      if (account.status != IrAuthenticatedAccountStatus::Succeeded ||
          !account.account) {
        continue;
      }
      if (stopToken.stop_requested()) {
        return std::nullopt;
      }
      if (request.target == IrExternalUrlTarget::Chart) {
        return chartUrlLookup(providerId, request.chart, runtime, stopToken);
      }
      if (request.target == IrExternalUrlTarget::Course) {
        return courseUrlLookup(providerId, IrCourseExternalUrlRequest{},
                               runtime, stopToken);
      }
      return std::nullopt;
    }
  } catch (...) {
  }
  return std::nullopt;
}

class IrExternalUrlService final {
public:
  using Resolver = std::function<std::optional<std::string>(
      const IrExternalUrlRequest &, std::stop_token)>;

  explicit IrExternalUrlService(Resolver resolver);
  ~IrExternalUrlService();

  IrExternalUrlService(const IrExternalUrlService &) = delete;
  IrExternalUrlService &operator=(const IrExternalUrlService &) = delete;

  [[nodiscard]] std::uint64_t open(IrExternalUrlRequest request);
  void close(std::uint64_t generation);
  [[nodiscard]] IrExternalUrlSnapshot snapshot() const;
  void stop() noexcept;

private:
  struct Work {
    IrExternalUrlRequest request;
    std::uint64_t generation = 0;
    std::stop_source stopSource;
  };

  void workerMain(std::stop_token) noexcept;

  Resolver resolver_;
  mutable std::mutex mutex_;
  std::condition_variable workAvailable_;
  std::optional<Work> pending_;
  std::stop_source activeStop_;
  IrExternalUrlSnapshot snapshot_;
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  std::jthread worker_;
};

} // namespace ir
