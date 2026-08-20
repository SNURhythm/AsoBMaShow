#pragma once

#include "../ThreadCompat.h"
#include "IrSubmissionService.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ir {

template <typename CredentialLookup, typename AccountLookup>
[[nodiscard]] inline std::string lookupFirstEnabledIrAccountName(
    const IrActiveProfileConfig &config, std::stop_token stopToken,
    CredentialLookup &&credentialLookup,
    AccountLookup &&accountLookup) noexcept {
  if (stopToken.stop_requested()) {
    return {};
  }
  try {
    for (const auto &[providerId, provider] : config.providers) {
      if (stopToken.stop_requested()) {
        return {};
      }
      if (!provider.enabled) {
        continue;
      }
      const std::string apiKey =
          credentialLookup(config.profileId, providerId);
      if (apiKey.empty()) {
        continue;
      }
      const auto account = accountLookup(
          providerId,
          {.profileId = config.profileId,
           .serverOrigin = provider.serverOrigin,
           .apiKey = apiKey},
          stopToken);
      if (account.status == IrAuthenticatedAccountStatus::Succeeded &&
          account.account) {
        return account.account->name;
      }
    }
  } catch (...) {
  }
  return {};
}

// Serializes account lookups onto a background worker. Replacing a request
// cancels the in-flight HTTP operation and only the newest request may publish
// its result, so profile activation never waits for an unreachable provider.
class IrAccountLookupService final {
public:
  using Lookup = std::function<std::string(const IrActiveProfileConfig &,
                                           std::stop_token)>;
  using Publish = std::function<void(std::string_view, std::string)>;

  IrAccountLookupService(Lookup lookup, Publish publish)
      : lookup_(std::move(lookup)), publish_(std::move(publish)),
        worker_([this](std::stop_token stopToken) { workerMain(stopToken); }) {}

  ~IrAccountLookupService() { stop(); }

  IrAccountLookupService(const IrAccountLookupService &) = delete;
  IrAccountLookupService &operator=(const IrAccountLookupService &) = delete;

  void request(IrActiveProfileConfig config) {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    activeRequestStop_.request_stop();
    activeRequestStop_ = std::stop_source{};
    pending_ = Request{.config = std::move(config),
                       .generation = ++generation_,
                       .stopSource = activeRequestStop_};
    workAvailable_.notify_one();
  }

  void stop() noexcept {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
      activeRequestStop_.request_stop();
      pending_.reset();
    }
    workAvailable_.notify_all();
    worker_.request_stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

private:
  struct Request {
    IrActiveProfileConfig config;
    std::uint64_t generation = 0;
    std::stop_source stopSource;
  };

  void workerMain(std::stop_token workerStopToken) noexcept {
    while (!workerStopToken.stop_requested()) {
      Request request;
      {
        std::unique_lock lock(mutex_);
        workAvailable_.wait(lock,
                            [this] { return stopping_ || pending_.has_value(); });
        if (stopping_) {
          return;
        }
        request = std::move(*pending_);
        pending_.reset();
      }

      std::string accountName;
      try {
        if (!request.stopSource.stop_requested()) {
          accountName = lookup_(request.config, request.stopSource.get_token());
        }
      } catch (...) {
        accountName.clear();
      }
      if (workerStopToken.stop_requested() || request.stopSource.stop_requested()) {
        continue;
      }

      // Hold the service lock across the short callback so a just-superseded
      // request cannot publish after its replacement is accepted.
      std::lock_guard lock(mutex_);
      if (!stopping_ && request.generation == generation_) {
        publish_(request.config.profileId, std::move(accountName));
      }
    }
  }

  Lookup lookup_;
  Publish publish_;
  std::mutex mutex_;
  std::condition_variable workAvailable_;
  std::optional<Request> pending_;
  std::stop_source activeRequestStop_;
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  std::jthread worker_;
};

} // namespace ir
