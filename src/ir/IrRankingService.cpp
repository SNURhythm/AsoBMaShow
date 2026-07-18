#include "IrRankingService.h"

#include "IrHttpClient.h"
#include "IrOutboxModels.h"
#include "IrProfileSettings.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace ir {
namespace {

using SteadyTimePoint = IrRankingServiceOptions::SteadyTimePoint;

constexpr std::size_t kMaximumRankingCacheEntries = 64;

SteadyTimePoint safeNow(const IrRankingServiceOptions &options) {
  try {
    return options.monotonicNow ? options.monotonicNow()
                                : std::chrono::steady_clock::now();
  } catch (...) {
    return std::chrono::steady_clock::now();
  }
}

std::string lookupCredential(const IrRankingServiceOptions &options,
                             std::string_view profileId,
                             std::string_view providerId) {
  try {
    return options.credentialLookup
               ? options.credentialLookup(profileId, providerId)
               : std::string{};
  } catch (...) {
    return {};
  }
}

std::string redactCredential(std::string value, std::string_view credential) {
  if (!credential.empty()) {
    std::size_t offset = 0;
    while ((offset = value.find(credential, offset)) != std::string::npos) {
      value.replace(offset, credential.size(), "[redacted]");
      offset += 10;
    }
  }
  return sanitizeDiagnostic(value);
}

bool matches(const IrRankingCacheKey &key,
             const IrRankingInvalidation &invalidation,
             const std::optional<std::string> &normalizedOrigin,
             const std::optional<std::string> &normalizedSha256) {
  return (!invalidation.profileId ||
          key.profileId == *invalidation.profileId) &&
         (!invalidation.providerId ||
          key.providerId == *invalidation.providerId) &&
         (!invalidation.serverOrigin ||
          (normalizedOrigin && key.serverOrigin == *normalizedOrigin)) &&
         (!invalidation.chartSha256 ||
          (normalizedSha256 && key.chartSha256 == *normalizedSha256));
}

std::optional<std::string> normalizedSha(std::string_view value) {
  if (value.size() != 64) {
    return std::nullopt;
  }
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    if (character >= 'A' && character <= 'F') {
      return static_cast<char>(character - 'A' + 'a');
    }
    return static_cast<char>(character);
  });
  if (!std::ranges::all_of(result, [](unsigned char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
      })) {
    return std::nullopt;
  }
  return result;
}

} // namespace

struct IrRankingService::Impl {
  struct CacheValue {
    std::shared_ptr<const IrChartRanking> ranking;
    SteadyTimePoint expiresAt;
  };

  struct Work {
    std::uint64_t generation = 0;
    IrRankingRequest request;
    IrRankingCacheKey key;
  };

  const IrDriverRegistry &drivers;
  IrHttpClient &http;
  IrRankingServiceOptions options;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::jthread worker;
  bool stopped = false;
  std::uint64_t generation = 0;
  std::uint64_t revision = 0;
  std::optional<Work> pending;
  std::optional<IrRankingCacheKey> activeKey;
  std::uint64_t activeGeneration = 0;
  std::stop_source activeStop;
  std::map<IrRankingCacheKey, CacheValue> cache;
  IrRankingSnapshot current;
  std::string activeProfileId;

  Impl(const IrDriverRegistry &driversValue, IrHttpClient &httpValue,
       IrRankingServiceOptions optionsValue)
      : drivers(driversValue), http(httpValue),
        options(std::move(optionsValue)) {
    worker = std::jthread(
        [this](std::stop_token stopToken) { workerMain(stopToken); });
  }

  void publishLocked(IrRankingSnapshotState state,
                     std::optional<IrRankingRequest> request,
                     std::shared_ptr<const IrChartRanking> ranking = {},
                     std::string diagnostic = {}, bool fromCache = false) {
    ++revision;
    current = {.revision = revision,
               .generation = request ? request->generation : generation,
               .state = state,
               .request = std::move(request),
               .ranking = std::move(ranking),
               .diagnostic = sanitizeDiagnostic(diagnostic),
               .fromCache = fromCache};
  }

  void pruneExpiredLocked(SteadyTimePoint now) {
    for (auto iterator = cache.begin(); iterator != cache.end();) {
      if (iterator->second.expiresAt <= now) {
        iterator = cache.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  std::uint64_t queue(IrRankingRequest request, bool bypassCache) {
    const auto built = makeIrRankingCacheKey(request);
    std::lock_guard lock(mutex);
    ++generation;
    request.generation = generation;
    activeStop.request_stop();
    pending.reset();
    if (stopped) {
      publishLocked(IrRankingSnapshotState::Closed, std::nullopt);
      return generation;
    }
    if (!built.value) {
      publishLocked(IrRankingSnapshotState::MalformedResponse,
                    std::move(request), {}, built.diagnostic);
      return generation;
    }

    request.serverOrigin = built.value->serverOrigin;
    request.chart.chartSha256 = built.value->chartSha256;
    const SteadyTimePoint now = safeNow(options);
    pruneExpiredLocked(now);
    if (bypassCache) {
      cache.erase(*built.value);
    } else {
      const auto found = cache.find(*built.value);
      if (found != cache.end() && found->second.expiresAt > now) {
        publishLocked(IrRankingSnapshotState::Succeeded, std::move(request),
                      found->second.ranking, {}, true);
        return generation;
      }
    }

    pending =
        Work{.generation = generation, .request = request, .key = *built.value};
    publishLocked(IrRankingSnapshotState::Loading, std::move(request));
    condition.notify_all();
    return generation;
  }

  void workerMain(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
      Work work;
      std::stop_token requestToken;
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] {
          return stopToken.stop_requested() || stopped || pending.has_value();
        });
        if (stopToken.stop_requested() || stopped) {
          break;
        }
        work = std::move(*pending);
        pending.reset();
        activeGeneration = work.generation;
        activeKey = work.key;
        activeStop = std::stop_source{};
        requestToken = activeStop.get_token();
      }

      const std::string credential = lookupCredential(
          options, work.request.profileId, work.request.providerId);
      ChartRankingOutcome outcome;
      if (credential.empty()) {
        outcome = {.status = ChartRankingStatus::AuthenticationRequired,
                   .diagnostic = "IR API key is required"};
      } else {
        const IrProviderRuntimeConfig runtime{
            .profileId = work.request.profileId,
            .serverOrigin = work.key.serverOrigin,
            .apiKey = credential};
        outcome = drivers.fetchChartRanking(work.request.providerId,
                                            work.request.chart, runtime, http,
                                            requestToken);
      }
      outcome.diagnostic =
          redactCredential(std::move(outcome.diagnostic), credential);

      std::lock_guard lock(mutex);
      if (activeGeneration == work.generation && activeKey == work.key) {
        activeGeneration = 0;
        activeKey.reset();
        condition.notify_all();
      }
      if (stopped || current.generation != work.generation ||
          current.state != IrRankingSnapshotState::Loading ||
          !current.request) {
        continue;
      }
      const auto currentKey = makeIrRankingCacheKey(*current.request);
      if (!currentKey.value || *currentKey.value != work.key) {
        continue;
      }

      if (outcome.status == ChartRankingStatus::Succeeded && outcome.ranking) {
        IrChartRanking normalized = std::move(*outcome.ranking);
        normalized.providerId = work.key.providerId;
        normalized.chart = work.request.chart;
        normalized.chart.chartSha256 = work.key.chartSha256;
        for (auto &entry : normalized.entries) {
          entry.playerName =
              redactCredential(std::move(entry.playerName), credential);
        }
        auto ranking =
            std::make_shared<const IrChartRanking>(std::move(normalized));
        pruneExpiredLocked(safeNow(options));
        if (!cache.contains(work.key) &&
            cache.size() >= kMaximumRankingCacheEntries) {
          cache.erase(cache.begin());
        }
        cache[work.key] = {.ranking = ranking,
                           .expiresAt = safeNow(options) + kIrRankingCacheTtl};
        publishLocked(IrRankingSnapshotState::Succeeded, work.request,
                      std::move(ranking));
        continue;
      }
      const ChartRankingStatus status =
          outcome.status == ChartRankingStatus::Succeeded
              ? ChartRankingStatus::MalformedResponse
              : outcome.status;
      publishLocked(snapshotStateFor(status), work.request, {},
                    outcome.diagnostic);
    }
  }
};

IrRankingService::IrRankingService(const IrDriverRegistry &drivers,
                                   IrHttpClient &http,
                                   IrRankingServiceOptions options)
    : impl_(std::make_unique<Impl>(drivers, http, std::move(options))) {}

IrRankingService::~IrRankingService() { stop(); }

std::uint64_t IrRankingService::open(IrRankingRequest request) {
  return impl_->queue(std::move(request), false);
}

std::uint64_t IrRankingService::refresh() {
  std::optional<IrRankingRequest> request;
  {
    std::lock_guard lock(impl_->mutex);
    request = impl_->current.request;
    if (!request) {
      return impl_->generation;
    }
  }
  return impl_->queue(std::move(*request), true);
}

std::uint64_t IrRankingService::refresh(IrRankingRequest request) {
  return impl_->queue(std::move(request), true);
}

void IrRankingService::close(std::uint64_t generation) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->current.generation != generation) {
    return;
  }
  impl_->activeStop.request_stop();
  if (impl_->pending && impl_->pending->generation == generation) {
    impl_->pending.reset();
  }
  impl_->publishLocked(IrRankingSnapshotState::Closed, std::nullopt);
}

void IrRankingService::pauseAndCancel() {
  std::unique_lock lock(impl_->mutex);
  impl_->activeStop.request_stop();
  impl_->pending.reset();
  if (impl_->current.state == IrRankingSnapshotState::Loading) {
    impl_->publishLocked(IrRankingSnapshotState::Cancelled,
                         impl_->current.request, {},
                         "IR ranking request was cancelled");
  }
  impl_->condition.wait(lock, [&] { return impl_->activeGeneration == 0; });
}

IrRankingSnapshot IrRankingService::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->current;
}

void IrRankingService::invalidate(const IrRankingInvalidation &invalidation) {
  const std::optional<std::string> origin =
      invalidation.serverOrigin
          ? normalizeServerOrigin(*invalidation.serverOrigin)
          : std::nullopt;
  const std::optional<std::string> sha256 =
      invalidation.chartSha256 ? normalizedSha(*invalidation.chartSha256)
                               : std::nullopt;
  std::lock_guard lock(impl_->mutex);
  for (auto iterator = impl_->cache.begin(); iterator != impl_->cache.end();) {
    if (matches(iterator->first, invalidation, origin, sha256)) {
      iterator = impl_->cache.erase(iterator);
    } else {
      ++iterator;
    }
  }

  const bool activeMatches =
      impl_->activeKey &&
      matches(*impl_->activeKey, invalidation, origin, sha256);
  const bool pendingMatches =
      impl_->pending &&
      matches(impl_->pending->key, invalidation, origin, sha256);
  if (activeMatches) {
    impl_->activeStop.request_stop();
  }
  if (pendingMatches) {
    impl_->pending.reset();
  }
  const auto currentKey =
      impl_->current.request
          ? makeIrRankingCacheKey(*impl_->current.request).value
          : std::nullopt;
  const bool currentMatches =
      currentKey && matches(*currentKey, invalidation, origin, sha256);
  if (currentMatches &&
      (invalidation.clearVisible || activeMatches || pendingMatches)) {
    impl_->publishLocked(IrRankingSnapshotState::Cancelled,
                         impl_->current.request, {},
                         "IR ranking state was invalidated");
  }
}

void IrRankingService::activateProfile(std::string_view profileId) {
  pauseAndCancel();
  std::lock_guard lock(impl_->mutex);
  impl_->activeStop.request_stop();
  impl_->pending.reset();
  impl_->cache.clear();
  impl_->activeProfileId = std::string(profileId);
  ++impl_->generation;
  impl_->publishLocked(IrRankingSnapshotState::Closed, std::nullopt);
}

void IrRankingService::stop() {
  std::jthread worker;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) {
      return;
    }
    impl_->stopped = true;
    impl_->activeStop.request_stop();
    impl_->pending.reset();
    impl_->cache.clear();
    ++impl_->generation;
    impl_->publishLocked(IrRankingSnapshotState::Closed, std::nullopt);
    worker = std::move(impl_->worker);
  }
  worker.request_stop();
  impl_->condition.notify_all();
  if (worker.joinable()) {
    worker.join();
  }
}

} // namespace ir
