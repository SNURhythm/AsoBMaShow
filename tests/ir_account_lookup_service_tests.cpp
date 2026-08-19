#include "ir/IrAccountLookupService.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool waitFor(std::condition_variable &condition, std::mutex &mutex,
             const std::function<bool()> &predicate) {
  std::unique_lock lock(mutex);
  return condition.wait_for(lock, 2s, predicate);
}

bool testLatestRequestCancelsAndPublishesOnlyLatestAccount() {
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool firstLookupStarted = false;
  std::atomic_bool firstLookupCancelled = false;
  std::vector<std::pair<std::string, std::string>> published;
  ir::IrAccountLookupService service(
      [&](const ir::IrActiveProfileConfig &config, std::stop_token stopToken) {
        if (config.profileId == "first") {
          firstLookupStarted.store(true, std::memory_order_release);
          condition.notify_all();
          while (!stopToken.stop_requested()) {
            std::this_thread::sleep_for(1ms);
          }
          firstLookupCancelled.store(true, std::memory_order_release);
          condition.notify_all();
          return std::string("stale");
        }
        return std::string("fresh");
      },
      [&](std::string_view profileId, std::string accountName) {
        std::lock_guard lock(mutex);
        published.emplace_back(profileId, std::move(accountName));
        condition.notify_all();
      });

  service.request({.profileId = "first"});
  if (!waitFor(condition, mutex, [&] {
        return firstLookupStarted.load(std::memory_order_acquire);
      })) {
    std::cerr << "first lookup did not start\n";
    return false;
  }
  service.request({.profileId = "second"});
  const bool completed = waitFor(condition, mutex, [&] {
    return firstLookupCancelled.load(std::memory_order_acquire) &&
           published.size() == 1;
  });
  service.stop();
  if (!completed || published.front().first != "second" ||
      published.front().second != "fresh") {
    std::cerr << "only latest account lookup may publish\n";
    return false;
  }
  return true;
}

} // namespace

int main() { return testLatestRequestCancelsAndPublishesOnlyLatestAccount() ? 0 : 1; }
