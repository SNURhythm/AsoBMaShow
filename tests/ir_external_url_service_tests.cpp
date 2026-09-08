#include "ir/IrExternalUrlService.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;

bool waitFor(std::condition_variable &condition, std::mutex &mutex,
             const std::function<bool()> &predicate) {
  std::unique_lock lock(mutex);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate()) {
    condition.wait_for(lock, 1ms);
    if (std::chrono::steady_clock::now() >= deadline) {
      return predicate();
    }
  }
  return true;
}

bool testOpenReturnsBeforeResolverCompletes() {
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool started = false;
  std::atomic_bool release = false;
  ir::IrExternalUrlService service(
      [&](const ir::IrExternalUrlRequest &, std::stop_token stopToken) {
        started.store(true, std::memory_order_release);
        condition.notify_all();
        while (!release.load(std::memory_order_acquire) &&
               !stopToken.stop_requested()) {
          std::this_thread::sleep_for(1ms);
        }
        return std::optional<std::string>("https://example.test/chart");
      });

  const auto before = std::chrono::steady_clock::now();
  const auto generation = service.open({});
  const auto elapsed = std::chrono::steady_clock::now() - before;
  if (generation == 0 || elapsed >= 100ms ||
      !waitFor(condition, mutex, [&] {
        return started.load(std::memory_order_acquire);
      })) {
    std::cerr << "opening an IR URL must not wait for the resolver\n";
    release.store(true, std::memory_order_release);
    service.stop();
    return false;
  }
  release.store(true, std::memory_order_release);
  condition.notify_all();
  service.stop();
  return true;
}

bool testReplacementCancelsStaleResult() {
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool firstStarted = false;
  ir::IrExternalUrlService service(
      [&](const ir::IrExternalUrlRequest &request,
          std::stop_token stopToken) -> std::optional<std::string> {
        if (request.profile.profileId == "first") {
          firstStarted.store(true, std::memory_order_release);
          condition.notify_all();
          while (!stopToken.stop_requested()) {
            std::this_thread::sleep_for(1ms);
          }
          return "https://example.test/stale";
        }
        return "https://example.test/fresh";
      });

  const auto firstGeneration =
      service.open({.profile = {.profileId = "first"}});
  if (firstGeneration == 0) {
    std::cerr << "first resolver was not queued\n";
    return false;
  }
  if (!waitFor(condition, mutex, [&] {
        return firstStarted.load(std::memory_order_acquire);
      })) {
    std::cerr << "first resolver did not start\n";
    return false;
  }
  const auto generation =
      service.open({.profile = {.profileId = "second"}});
  if (!waitFor(condition, mutex, [&] {
        const auto snapshot = service.snapshot();
        return snapshot.generation == generation && snapshot.finished;
      })) {
    std::cerr << "replacement resolver did not finish\n";
    return false;
  }
  const auto snapshot = service.snapshot();
  service.stop();
  if (snapshot.url != "https://example.test/fresh") {
    std::cerr << "stale IR URL was published\n";
    return false;
  }
  return true;
}

bool testResolutionMatchesProviderOrderAndAuthentication() {
  ir::IrExternalUrlRequest request{
      .profile =
          {.profileId = "profile",
           .providers =
               {{"01-disabled", {.enabled = false}},
                {"02-missing", {.enabled = true}},
                {"03-unauthenticated", {.enabled = true}},
                {"04-authenticated", {.enabled = true}}}},
      .target = ir::IrExternalUrlTarget::Chart,
      .chart = {.keyMode = 7, .chartSha256 = "sha"},
  };
  std::vector<std::string> credentials;
  std::vector<std::string> accounts;
  std::vector<std::string> urls;
  const auto resolved = ir::resolveFirstEnabledIrExternalUrl(
      request, {},
      [&](std::string_view, std::string_view providerId) {
        credentials.emplace_back(providerId);
        return providerId == "02-missing" ? std::string{}
                                            : std::string("key");
      },
      [&](std::string_view providerId, const ir::IrProviderRuntimeConfig &,
          std::stop_token) {
        accounts.emplace_back(providerId);
        if (providerId != "04-authenticated") {
          return ir::IrAuthenticatedAccountOutcome{};
        }
        return ir::IrAuthenticatedAccountOutcome{
            .status = ir::IrAuthenticatedAccountStatus::Succeeded,
            .account = ir::IrAuthenticatedAccount{.name = "player"}};
      },
      [&](std::string_view providerId,
          const ir::IrChartExternalUrlRequest &chart,
          const ir::IrProviderRuntimeConfig &, std::stop_token) {
        urls.emplace_back(providerId);
        return chart.chartSha256 == "sha"
                   ? std::optional<std::string>("https://example.test/sha")
                   : std::nullopt;
      },
      [](std::string_view, const ir::IrCourseExternalUrlRequest &,
         const ir::IrProviderRuntimeConfig &, std::stop_token) {
        return std::optional<std::string>{};
      });
  return resolved == "https://example.test/sha" &&
         credentials ==
             std::vector<std::string>{"02-missing", "03-unauthenticated",
                                      "04-authenticated"} &&
         accounts ==
             std::vector<std::string>{"03-unauthenticated",
                                      "04-authenticated"} &&
         urls == std::vector<std::string>{"04-authenticated"};
}

} // namespace

int main() {
  return testOpenReturnsBeforeResolverCompletes() &&
                 testReplacementCancelsStaleResult() &&
                 testResolutionMatchesProviderOrderAndAuthentication()
             ? 0
             : 1;
}
