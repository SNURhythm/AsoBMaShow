#include "ir/IrRankingService.h"

#include "ir/IrHttpClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <class T>
concept HasApiKeyMember = requires(T value) { value.apiKey; };

class FakeHttpClient final : public ir::IrHttpClient {
public:
  ir::IrHttpResponse perform(const ir::IrHttpRequest &,
                             std::stop_token) noexcept override {
    return {.transportError = ir::IrTransportError::Other,
            .diagnostic = "fake ranking driver should not use HTTP"};
  }
};

class Gate {
public:
  explicit Gate(bool ignoreCancellation = false)
      : ignoreCancellation_(ignoreCancellation) {}

  bool wait(std::stop_token token) {
    std::unique_lock lock(mutex_);
    if (ignoreCancellation_) {
      condition_.wait(lock, [&] { return open_; });
      return true;
    }
    condition_.wait(lock, token,
                    [&] { return open_ || token.stop_requested(); });
    return open_ && !token.stop_requested();
  }

  void open() {
    std::lock_guard lock(mutex_);
    open_ = true;
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable_any condition_;
  bool open_ = false;
  bool ignoreCancellation_ = false;
};

struct RankingAction {
  ir::ChartRankingOutcome outcome;
  std::shared_ptr<Gate> gate;
};

struct RankingCall {
  ir::IrChartQuery chart;
  std::string profileId;
  std::string serverOrigin;
  std::string credential;
  std::optional<std::string> pageToken;
};

class FakeRankingDriver final : public ir::IrDriver {
public:
  explicit FakeRankingDriver(std::string providerId)
      : providerId_(std::move(providerId)) {}

  std::string_view providerId() const noexcept override { return providerId_; }

  ir::IrDriverCapabilities capabilities() const noexcept override {
    return {.readOnly = true, .chartRankings = true};
  }

  ir::ChartRankingOutcome fetchChartRanking(
      const ir::IrChartQuery &query, const ir::IrProviderRuntimeConfig &config,
      ir::IrHttpClient &, std::stop_token stopToken) const override {
    RankingAction action;
    {
      std::lock_guard lock(mutex_);
      calls_.push_back({.chart = query,
                        .profileId = config.profileId,
                        .serverOrigin = config.serverOrigin,
                        .credential = config.apiKey});
      if (!actions_.empty()) {
        action = std::move(actions_.front());
        actions_.pop_front();
      } else {
        action.outcome = success(providerId_, query, "default");
      }
      callsChanged_.notify_all();
    }
    if (action.gate && !action.gate->wait(stopToken)) {
      return {.status = ir::ChartRankingStatus::Cancelled,
              .diagnostic = "cancelled"};
    }
    return action.outcome;
  }

  ir::ChartRankingOutcome fetchChartRankingPage(
      const ir::IrChartQuery &query, std::string_view pageToken,
      const ir::IrProviderRuntimeConfig &config, ir::IrHttpClient &,
      std::stop_token stopToken) const override {
    RankingAction action;
    {
      std::lock_guard lock(mutex_);
      calls_.push_back({.chart = query,
                        .profileId = config.profileId,
                        .serverOrigin = config.serverOrigin,
                        .credential = config.apiKey,
                        .pageToken = std::string(pageToken)});
      if (!actions_.empty()) {
        action = std::move(actions_.front());
        actions_.pop_front();
      }
      callsChanged_.notify_all();
    }
    if (action.gate && !action.gate->wait(stopToken)) {
      return {.status = ir::ChartRankingStatus::Cancelled,
              .diagnostic = "cancelled"};
    }
    return action.outcome;
  }

  void push(RankingAction action) {
    std::lock_guard lock(mutex_);
    actions_.push_back(std::move(action));
  }

  bool waitForCalls(std::size_t count) const {
    std::unique_lock lock(mutex_);
    return callsChanged_.wait_for(lock, 3s,
                                  [&] { return calls_.size() >= count; });
  }

  std::vector<RankingCall> calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }

  static ir::ChartRankingOutcome success(std::string_view providerId,
                                         const ir::IrChartQuery &query,
                                         std::string player,
                                         std::optional<std::string> next = {}) {
    ir::IrChartRanking ranking{
        .providerId = std::string(providerId),
        .chart = query,
        .entries = {{.rank = 1,
                     .providerEntryId = player,
                     .playerName = std::move(player),
                     .score = 1500,
                     .maxScore = query.totalNotes * 2,
                     .clearType = kClearTypeNormalClearRank}},
        .nextPageToken = std::move(next),
        .fetchedAtUnixMillis = 1234};
    return {.status = ir::ChartRankingStatus::Succeeded,
            .ranking = std::move(ranking)};
  }

private:
  std::string providerId_;
  mutable std::mutex mutex_;
  mutable std::condition_variable callsChanged_;
  mutable std::deque<RankingAction> actions_;
  mutable std::vector<RankingCall> calls_;
};

std::string sha(char value) { return std::string(64, value); }

ir::IrRankingRequest request(std::string profileId = "profile-a",
                             std::string providerId = "fake",
                             std::string origin = "https://rank.example.test",
                             int keyMode = 7, std::string chartSha = sha('a'),
                             int totalNotes = 1000,
                             std::string comparisonLabel = "This Play") {
  return {.profileId = std::move(profileId),
          .providerId = std::move(providerId),
          .serverOrigin = std::move(origin),
          .chart = {.keyMode = keyMode,
                    .chartMd5 = std::string(32, 'b'),
                    .chartSha256 = std::move(chartSha),
                    .totalNotes = totalNotes},
          .localComparison =
              ir::IrLocalComparison{.label = std::move(comparisonLabel),
                                    .score = 1200,
                                    .maxScore = totalNotes * 2,
                                    .clearType = kClearTypeEasyClearRank,
                                    .badPoints = 12,
                                    .maxCombo = 456}};
}

class Harness {
public:
  Harness() {
    addDriver("fake");
    addDriver("other");
    credentials["profile-a|fake"] = "sentinel-api-key";
    credentials["profile-a|other"] = "other-key";
    credentials["profile-b|fake"] = "profile-b-key";
    ir::IrRankingServiceOptions options;
    options.monotonicNow = [this] {
      return std::chrono::steady_clock::time_point(
          std::chrono::milliseconds(nowMillis.load()));
    };
    options.credentialLookup = [this](std::string_view profileId,
                                      std::string_view providerId) {
      std::lock_guard lock(credentialsMutex);
      const auto found = credentials.find(std::string(profileId) + "|" +
                                          std::string(providerId));
      return found == credentials.end() ? std::string{} : found->second;
    };
    service = std::make_unique<ir::IrRankingService>(registry, http,
                                                     std::move(options));
  }

  ~Harness() {
    if (service) {
      service->stop();
    }
  }

  std::shared_ptr<FakeRankingDriver> driver(std::string_view providerId) {
    return drivers.at(std::string(providerId));
  }

  bool waitFor(std::uint64_t generation,
               ir::IrRankingSnapshotState state) const {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto value = service->snapshot();
      if (value.generation == generation && value.state == state) {
        return true;
      }
      std::this_thread::yield();
    }
    return false;
  }

  std::uint64_t openAndWait(ir::IrRankingRequest value,
                            ir::IrRankingSnapshotState state =
                                ir::IrRankingSnapshotState::Succeeded) {
    const auto generation = service->open(std::move(value));
    expect(waitFor(generation, state),
           "ranking request reaches expected state");
    return generation;
  }

  void setCredential(std::string_view profileId, std::string_view providerId,
                     std::string credential) {
    std::lock_guard lock(credentialsMutex);
    const std::string key =
        std::string(profileId) + "|" + std::string(providerId);
    if (credential.empty()) {
      credentials.erase(key);
    } else {
      credentials[key] = std::move(credential);
    }
  }

  std::atomic<std::int64_t> nowMillis{1000};
  ir::IrDriverRegistry registry;
  FakeHttpClient http;
  std::map<std::string, std::shared_ptr<FakeRankingDriver>, std::less<>>
      drivers;
  std::mutex credentialsMutex;
  std::map<std::string, std::string, std::less<>> credentials;
  std::unique_ptr<ir::IrRankingService> service;

private:
  void addDriver(std::string providerId) {
    auto value = std::make_shared<FakeRankingDriver>(providerId);
    std::string diagnostic;
    expect(registry.registerDriver(value, diagnostic),
           "ranking test driver registers");
    drivers.emplace(std::move(providerId), std::move(value));
  }
};

void testFetchCacheExpiryAndRefresh() {
  Harness harness;
  auto driver = harness.driver("fake");
  driver->push({.outcome = FakeRankingDriver::success(
                    "wrong-provider", request().chart, "first")});
  const auto firstGeneration = harness.openAndWait(request());
  auto snapshot = harness.service->snapshot();
  expect(snapshot.generation == firstGeneration && !snapshot.fromCache &&
             snapshot.request && snapshot.request->localComparison &&
             snapshot.request->localComparison->label == "This Play" &&
             snapshot.ranking && snapshot.ranking->providerId == "fake" &&
             snapshot.ranking->entries.front().playerName == "first",
         "open fetches and normalizes a remote ranking while retaining local "
         "comparison");
  expect(driver->calls().size() == 1 &&
             driver->calls().front().credential == "sentinel-api-key",
         "credential is loaded only for request execution");

  auto cachedRequest = request();
  cachedRequest.serverOrigin = "https://RANK.example.test:443/";
  cachedRequest.localComparison->label = "Personal Best";
  const auto cachedGeneration = harness.service->open(cachedRequest);
  snapshot = harness.service->snapshot();
  expect(snapshot.generation == cachedGeneration &&
             snapshot.state == ir::IrRankingSnapshotState::Succeeded &&
             snapshot.fromCache &&
             snapshot.request->serverOrigin == "https://rank.example.test" &&
             snapshot.request->localComparison->label == "Personal Best" &&
             driver->calls().size() == 1,
         "fresh cache is reused by normalized origin with the new comparison "
         "snapshot");

  harness.nowMillis += 5 * 60 * 1000 - 1;
  harness.openAndWait(request());
  expect(driver->calls().size() == 1,
         "ranking cache remains fresh until five minutes");

  harness.nowMillis += 1;
  driver->push({.outcome = FakeRankingDriver::success("fake", request().chart,
                                                      "expired")});
  harness.openAndWait(request());
  expect(driver->calls().size() == 2 &&
             harness.service->snapshot().ranking->entries.front().playerName ==
                 "expired",
         "five-minute expiry performs another fetch");

  driver->push({.outcome = FakeRankingDriver::success("fake", request().chart,
                                                      "refreshed")});
  const auto refreshGeneration = harness.service->refresh();
  expect(harness.waitFor(refreshGeneration,
                         ir::IrRankingSnapshotState::Succeeded) &&
             driver->calls().size() == 3 &&
             !harness.service->snapshot().fromCache &&
             harness.service->snapshot().ranking->entries.front().playerName ==
                 "refreshed",
         "refresh bypasses and replaces a fresh cache entry");
}

void testFailuresAreNotCachedAndMissingCredentialCallsDriver() {
  const std::vector<ir::ChartRankingStatus> statuses{
      ir::ChartRankingStatus::ChartNotFound,
      ir::ChartRankingStatus::AuthenticationRequired,
      ir::ChartRankingStatus::TransientFailure,
      ir::ChartRankingStatus::Unsupported,
      ir::ChartRankingStatus::MalformedResponse,
      ir::ChartRankingStatus::OversizedResponse,
      ir::ChartRankingStatus::Cancelled};
  for (const auto status : statuses) {
    Harness harness;
    auto driver = harness.driver("fake");
    driver->push({.outcome = {.status = status, .diagnostic = "failed"}});
    const auto first = harness.service->open(request());
    expect(harness.waitFor(first, ir::snapshotStateFor(status)),
           "driver failure is published");
    driver->push({.outcome = FakeRankingDriver::success("fake", request().chart,
                                                        "retry")});
    harness.openAndWait(request());
    expect(driver->calls().size() == 2,
           "non-success ranking outcomes are not cached");
  }

  Harness missing;
  missing.setCredential("profile-a", "fake", {});
  missing.openAndWait(request());
  const auto calls = missing.driver("fake")->calls();
  expect(calls.size() == 1 && calls.front().credential.empty() &&
             missing.service->snapshot().state ==
                 ir::IrRankingSnapshotState::Succeeded,
         "missing execution-time credential is delegated as an anonymous "
         "ranking request");
}

void testLatestRequestCloseAndLateCompletion() {
  Harness harness;
  auto driver = harness.driver("fake");
  const auto slowGate = std::make_shared<Gate>(true);
  auto slowRequest = request();
  driver->push({.outcome = FakeRankingDriver::success("fake", slowRequest.chart,
                                                      "stale"),
                .gate = slowGate});
  const auto slowGeneration = harness.service->open(slowRequest);
  expect(driver->waitForCalls(1), "slow ranking request starts");

  auto latestRequest = request();
  latestRequest.chart.chartSha256 = sha('c');
  driver->push({.outcome = FakeRankingDriver::success(
                    "fake", latestRequest.chart, "latest")});
  const auto latestGeneration = harness.service->open(latestRequest);
  slowGate->open();
  expect(
      harness.waitFor(latestGeneration, ir::IrRankingSnapshotState::Succeeded),
      "latest ranking request completes after stale request exits");
  const auto snapshot = harness.service->snapshot();
  expect(snapshot.generation == latestGeneration && snapshot.ranking &&
             snapshot.ranking->chart.chartSha256 == sha('c') &&
             snapshot.ranking->entries.front().playerName == "latest" &&
             snapshot.generation != slowGeneration,
         "late completion cannot replace the latest request identity");

  const auto closeGate = std::make_shared<Gate>(true);
  driver->push({.outcome = FakeRankingDriver::success("fake", request().chart,
                                                      "after-close"),
                .gate = closeGate});
  const auto closeGeneration = harness.service->open(request());
  expect(driver->waitForCalls(3), "close fixture request starts");
  harness.service->close(closeGeneration);
  expect(harness.service->snapshot().state ==
             ir::IrRankingSnapshotState::Closed,
         "close is immediate and publishes closed state");
  closeGate->open();
  std::this_thread::sleep_for(20ms);
  expect(harness.service->snapshot().state ==
             ir::IrRankingSnapshotState::Closed,
         "completion after close is rejected");
}

void testPaginationAppendsWithoutDiscardingVisibleRows() {
  Harness harness;
  auto driver = harness.driver("fake");
  driver->push({.outcome = FakeRankingDriver::success(
                    "fake", request().chart, "first", "page-2")});
  const auto generation = harness.openAndWait(request());

  auto nextPage = FakeRankingDriver::success("fake", request().chart,
                                             "second");
  nextPage.ranking->entries.front().rank = 2;
  const auto gate = std::make_shared<Gate>();
  driver->push({.outcome = std::move(nextPage), .gate = gate});

  expect(harness.service->loadNextPage(generation),
         "a visible continuation page is queued once");
  expect(driver->waitForCalls(2), "continuation page starts");
  auto loading = harness.service->snapshot();
  expect(loading.state == ir::IrRankingSnapshotState::Succeeded &&
             loading.loadingNextPage && loading.ranking &&
             loading.ranking->entries.size() == 1 &&
             loading.ranking->entries.front().playerName == "first",
         "loading the next page keeps the current virtualized rows visible");
  expect(!harness.service->loadNextPage(generation),
         "duplicate near-end notifications do not queue duplicate pages");

  gate->open();
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         harness.service->snapshot().loadingNextPage) {
    std::this_thread::yield();
  }
  const auto appended = harness.service->snapshot();
  expect(appended.state == ir::IrRankingSnapshotState::Succeeded &&
             !appended.loadingNextPage && !appended.paginationBlocked &&
             appended.ranking && appended.ranking->entries.size() == 2 &&
             appended.ranking->entries[0].playerName == "first" &&
             appended.ranking->entries[1].playerName == "second" &&
             !appended.ranking->nextPageToken.has_value(),
         "a continuation page is appended in order and completes pagination");
  const auto calls = driver->calls();
  expect(calls.size() == 2 && !calls[0].pageToken &&
             calls[1].pageToken == "page-2",
         "the service delegates continuation through the page method only");
}

void testPaginationFailureKeepsRowsAndStopsAutomaticRetry() {
  Harness harness;
  auto driver = harness.driver("fake");
  driver->push({.outcome = FakeRankingDriver::success(
                    "fake", request().chart, "first", "page-2")});
  const auto generation = harness.openAndWait(request());
  driver->push({.outcome = {.status = ir::ChartRankingStatus::TransientFailure,
                            .diagnostic = "offline"}});
  expect(harness.service->loadNextPage(generation),
         "failing continuation page is queued");

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         !harness.service->snapshot().paginationBlocked) {
    std::this_thread::yield();
  }
  const auto failed = harness.service->snapshot();
  expect(failed.state == ir::IrRankingSnapshotState::Succeeded &&
             !failed.loadingNextPage && failed.paginationBlocked &&
             failed.ranking && failed.ranking->entries.size() == 1 &&
             failed.diagnostic == "offline",
         "continuation failure preserves rows and exposes a retryable status");
  expect(!harness.service->loadNextPage(generation) &&
             driver->calls().size() == 2,
         "near-end updates cannot create an automatic retry loop");
}

void testPaginationRejectsDuplicateProviderRows() {
  Harness harness;
  auto driver = harness.driver("fake");
  driver->push({.outcome = FakeRankingDriver::success(
                    "fake", request().chart, "duplicate", "page-2")});
  const auto generation = harness.openAndWait(request());
  auto duplicate =
      FakeRankingDriver::success("fake", request().chart, "duplicate");
  duplicate.ranking->entries.front().rank = 2;
  driver->push({.outcome = std::move(duplicate)});
  expect(harness.service->loadNextPage(generation),
         "duplicate continuation fixture is queued");

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline &&
         !harness.service->snapshot().paginationBlocked) {
    std::this_thread::yield();
  }
  const auto failed = harness.service->snapshot();
  expect(failed.state == ir::IrRankingSnapshotState::Succeeded &&
             failed.paginationBlocked && failed.ranking &&
             failed.ranking->entries.size() == 1,
         "a duplicated provider row cannot corrupt the appended leaderboard");
}

void testCloseRejectsLatePaginationCompletion() {
  Harness harness;
  auto driver = harness.driver("fake");
  driver->push({.outcome = FakeRankingDriver::success(
                    "fake", request().chart, "first", "page-2")});
  const auto generation = harness.openAndWait(request());
  const auto gate = std::make_shared<Gate>(true);
  auto next = FakeRankingDriver::success("fake", request().chart, "second");
  next.ranking->entries.front().rank = 2;
  driver->push({.outcome = std::move(next), .gate = gate});
  expect(harness.service->loadNextPage(generation),
         "late pagination fixture is queued");
  expect(driver->waitForCalls(2), "late pagination fixture starts");

  harness.service->close(generation);
  expect(harness.service->snapshot().state ==
             ir::IrRankingSnapshotState::Closed,
         "closing during pagination immediately closes the visible snapshot");
  gate->open();
  std::this_thread::sleep_for(20ms);
  expect(harness.service->snapshot().state ==
             ir::IrRankingSnapshotState::Closed,
         "a late page completion cannot reopen a closed ranking modal");
}

void testCacheIdentityAndCredentialFreeDebugTypes() {
  static_assert(!HasApiKeyMember<ir::IrRankingCacheKey>);
  static_assert(!HasApiKeyMember<ir::IrChartRanking>);

  const auto base = ir::makeIrRankingCacheKey(request());
  expect(base.value.has_value(), "valid request builds cache key");
  const auto normalized = ir::makeIrRankingCacheKey(
      request("profile-a", "fake", "https://RANK.example.test:443/"));
  expect(normalized.value == base.value,
         "cache key normalizes equivalent server origins");
  expect(!ir::makeIrRankingCacheKey(request("profile-a", "bad.provider"))
              .value,
         "cache key rejects provider IDs that durable IR layers reject");

  std::vector<ir::IrRankingRequest> variants;
  variants.push_back(request("profile-b"));
  variants.push_back(request("profile-a", "other"));
  variants.push_back(request("profile-a", "fake", "https://other.example"));
  variants.push_back(
      request("profile-a", "fake", "https://rank.example.test", 14));
  variants.push_back(
      request("profile-a", "fake", "https://rank.example.test", 7, sha('c')));
  variants.push_back(request("profile-a", "fake", "https://rank.example.test",
                             7, sha('a'), 1001));
  for (const auto &variant : variants) {
    const auto key = ir::makeIrRankingCacheKey(variant);
    expect(key.value && key.value != base.value,
           "every cache identity dimension separates entries");
  }
  const std::string keyDebug = ir::describeIrRankingCacheKey(*base.value);
  const std::string valueDebug = ir::describeIrChartRanking(
      *FakeRankingDriver::success("fake", request().chart, "player").ranking);
  expect(keyDebug.find("sentinel-api-key") == std::string::npos &&
             valueDebug.find("sentinel-api-key") == std::string::npos,
         "cache key/value debug serialization contains no credential field or "
         "value");

  Harness hostile;
  auto hostileOutcome = FakeRankingDriver::success(
      "fake", request().chart, "sentinel-api-key");
  hostileOutcome.ranking->entries.front().providerEntryId = "safe-player-id";
  hostile.driver("fake")->push({.outcome = std::move(hostileOutcome)});
  hostile.openAndWait(request());
  const auto hostileSnapshot = hostile.service->snapshot();
  expect(
      hostileSnapshot.ranking &&
          hostileSnapshot.ranking->entries.front().playerName == "[redacted]",
      "provider data cannot echo the current API key into the ranking cache");

  Harness hostileToken;
  hostileToken.driver("fake")->push(
      {.outcome = FakeRankingDriver::success(
           "fake", request().chart, "player", "sentinel-api-key")});
  const auto hostileTokenGeneration = hostileToken.service->open(request());
  expect(hostileToken.waitFor(hostileTokenGeneration,
                              ir::IrRankingSnapshotState::MalformedResponse) &&
             !hostileToken.service->snapshot().ranking,
         "a provider cannot echo the API key into a cached page token");

  Harness hostileIdentity;
  auto hostileIdentityOutcome =
      FakeRankingDriver::success("fake", request().chart, "player");
  hostileIdentityOutcome.ranking->entries.front().providerEntryId =
      "sentinel-api-key";
  hostileIdentity.driver("fake")->push(
      {.outcome = std::move(hostileIdentityOutcome)});
  const auto hostileIdentityGeneration = hostileIdentity.service->open(request());
  expect(hostileIdentity.waitFor(
             hostileIdentityGeneration,
             ir::IrRankingSnapshotState::MalformedResponse) &&
             !hostileIdentity.service->snapshot().ranking,
         "a provider cannot echo the API key into a cached row identity");

  Harness harness;
  auto fake = harness.driver("fake");
  auto other = harness.driver("other");
  harness.openAndWait(request());
  harness.openAndWait(request("profile-b"));
  harness.openAndWait(request("profile-a", "other"));
  harness.openAndWait(request("profile-a", "fake", "https://other.example"));
  harness.openAndWait(
      request("profile-a", "fake", "https://rank.example.test", 14));
  harness.openAndWait(
      request("profile-a", "fake", "https://rank.example.test", 7, sha('c')));
  harness.openAndWait(request("profile-a", "fake", "https://rank.example.test",
                              7, sha('a'), 1001));
  expect(fake->calls().size() == 6 && other->calls().size() == 1,
         "profile, provider, origin, mode, SHA-256, and notes all separate "
         "cache fetches");
}

void testInvalidationAndShutdown() {
  Harness harness;
  auto driver = harness.driver("fake");
  const auto chartA = request();
  auto chartB = request();
  chartB.chart.chartSha256 = sha('c');
  harness.openAndWait(chartA);
  harness.openAndWait(chartB);
  expect(driver->calls().size() == 2, "invalidation fixtures populate cache");

  harness.service->invalidate({.profileId = "profile-a",
                               .providerId = "fake",
                               .serverOrigin = "https://rank.example.test",
                               .chartSha256 = sha('a')});
  harness.openAndWait(chartA);
  const auto chartBGeneration = harness.service->open(chartB);
  expect(harness.waitFor(chartBGeneration,
                         ir::IrRankingSnapshotState::Succeeded) &&
             harness.service->snapshot().fromCache &&
             driver->calls().size() == 3,
         "successful-submission invalidation removes only the matching chart "
         "identity");

  harness.service->invalidate({.profileId = "profile-a", .providerId = "fake"});
  harness.openAndWait(chartA);
  expect(
      driver->calls().size() == 4,
      "provider disable or credential change clears affected provider cache");

  harness.service->invalidate({.profileId = "profile-a",
                               .providerId = "fake",
                               .serverOrigin = "https://rank.example.test"});
  harness.openAndWait(chartA);
  expect(driver->calls().size() == 5,
         "origin change clears affected origin cache");

  const auto gate = std::make_shared<Gate>();
  driver->push(
      {.outcome = FakeRankingDriver::success("fake", chartB.chart, "cancelled"),
       .gate = gate});
  const auto activeGeneration = harness.service->refresh(chartB);
  expect(driver->waitForCalls(6), "active invalidation fixture starts");
  harness.service->invalidate({.profileId = "profile-a", .providerId = "fake"});
  expect(
      harness.waitFor(activeGeneration, ir::IrRankingSnapshotState::Cancelled),
      "configuration invalidation cancels matching active request");

  harness.service->activateProfile("profile-b");
  expect(harness.service->snapshot().state ==
             ir::IrRankingSnapshotState::Closed,
         "profile switch cancels reads and clears visible ranking state");
  harness.openAndWait(request("profile-a"));
  expect(driver->calls().size() == 7,
         "profile activation clears prior profile caches");

  harness.service->stop();
  const auto callsBeforeStoppedOpen = driver->calls().size();
  const auto stoppedGeneration = harness.service->open(request());
  expect(harness.service->snapshot().generation == stoppedGeneration &&
             harness.service->snapshot().state ==
                 ir::IrRankingSnapshotState::Closed &&
             driver->calls().size() == callsBeforeStoppedOpen,
         "shutdown cancels work, clears cache, and rejects new fetches");
}

void testPauseKeepsRankingWorkQuiescedUntilProfileReactivation() {
  Harness harness;
  auto driver = harness.driver("fake");
  const auto oldAccountGate = std::make_shared<Gate>(true);
  auto oldAccountRequest = request();
  driver->push({.outcome = FakeRankingDriver::success(
                    "fake", oldAccountRequest.chart, "old-account"),
                .gate = oldAccountGate});
  const auto oldGeneration = harness.service->open(oldAccountRequest);
  expect(driver->waitForCalls(1),
         "old-account ranking request reaches the blocked driver");

  std::atomic_bool pauseReturned{false};
  std::thread pausing([&] {
    harness.service->pauseAndCancel();
    pauseReturned.store(true);
  });
  expect(harness.waitFor(oldGeneration, ir::IrRankingSnapshotState::Cancelled),
         "ranking pause publishes cancellation while waiting for old work");

  const auto callsBeforePausedOpen = driver->calls().size();
  auto pausedRequest = request();
  pausedRequest.chart.chartSha256 = sha('e');
  const auto pausedGeneration = harness.service->open(pausedRequest);
  const auto pausedSnapshot = harness.service->snapshot();
  expect(pausedSnapshot.generation == pausedGeneration &&
             pausedSnapshot.state == ir::IrRankingSnapshotState::Closed &&
             driver->calls().size() == callsBeforePausedOpen &&
             !pauseReturned.load(),
         "paused ranking service rejects new work through credential mutation");

  oldAccountGate->open();
  pausing.join();
  expect(pauseReturned.load(),
         "ranking pause returns only after old-account work is quiescent");

  harness.service->activateProfile("profile-a");
  driver->push({.outcome = FakeRankingDriver::success(
                    "fake", pausedRequest.chart, "reactivated")});
  harness.openAndWait(pausedRequest);
  expect(driver->calls().size() == callsBeforePausedOpen + 1,
         "profile reactivation admits ranking work again");
}

void testPauseRejectsStalePagination() {
  Harness harness;
  auto driver = harness.driver("fake");
  driver->push({.outcome = FakeRankingDriver::success(
                    "fake", request().chart, "first", "page-2")});
  const auto oldGeneration = harness.openAndWait(request());
  const auto callsBeforePause = driver->calls().size();

  harness.service->pauseAndCancel();

  expect(!harness.service->loadNextPage(oldGeneration),
         "paused ranking service rejects stale continuation generation");
  std::this_thread::sleep_for(20ms);
  expect(driver->calls().size() == callsBeforePause,
         "stale continuation cannot reach the old-account driver");
}

} // namespace

int main() {
  static_assert(ir::kIrRankingCacheTtl == std::chrono::minutes(5));
  testFetchCacheExpiryAndRefresh();
  testFailuresAreNotCachedAndMissingCredentialCallsDriver();
  testLatestRequestCloseAndLateCompletion();
  testPaginationAppendsWithoutDiscardingVisibleRows();
  testPaginationFailureKeepsRowsAndStopsAutomaticRetry();
  testPaginationRejectsDuplicateProviderRows();
  testCloseRejectsLatePaginationCompletion();
  testCacheIdentityAndCredentialFreeDebugTypes();
  testInvalidationAndShutdown();
  testPauseKeepsRankingWorkQuiescedUntilProfileReactivation();
  testPauseRejectsStalePagination();

  if (failures != 0) {
    std::cerr << failures << " IR ranking service test(s) failed\n";
    return 1;
  }
  std::cout << "IR ranking service tests passed\n";
  return 0;
}
