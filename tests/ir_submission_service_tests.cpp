#include "ir/IrSubmissionService.h"

#include "ir/IrHttpClient.h"
#include "repositories/ReplayRepository.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
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

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-ir-service-" + std::to_string(++sequence) + "-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

class FakeHttpClient final : public ir::IrHttpClient {
public:
  ir::IrHttpResponse perform(const ir::IrHttpRequest &,
                             std::stop_token) noexcept override {
    return {.transportError = ir::IrTransportError::Other,
            .diagnostic = "fake driver should not use HTTP"};
  }
};

struct DriverCall {
  bool poll = false;
  bool userIntent = false;
  std::string configuredOrigin;
  std::string apiKey;
  std::string remoteJobId;
  std::string remoteOrigin;
};

class FakeDriver final : public ir::IrDriver {
public:
  explicit FakeDriver(
      ir::IrDriverCapabilities capabilities = {.readOnly = false,
                                               .chartRankings = false,
                                               .scoreSubmission = true,
                                               .deferredSubmission = true})
      : capabilities_(capabilities) {}

  std::string_view providerId() const noexcept override { return "fake"; }
  ir::IrDriverCapabilities capabilities() const noexcept override {
    return capabilities_;
  }

  ir::DeliveryOutcome submit(const ir::IrOutboxEntry &entry,
                             const ir::IrProviderRuntimeConfig &config,
                             ir::IrHttpClient &,
                             std::stop_token token) const override {
    return perform(false, entry, config, token);
  }

  ir::DeliveryOutcome poll(const ir::IrOutboxEntry &entry,
                           const ir::IrProviderRuntimeConfig &config,
                           ir::IrHttpClient &,
                           std::stop_token token) const override {
    return perform(true, entry, config, token);
  }

  void pushSubmit(ir::DeliveryOutcome outcome) {
    std::lock_guard lock(mutex_);
    submitOutcomes_.push_back(std::move(outcome));
  }

  void pushPoll(ir::DeliveryOutcome outcome) {
    std::lock_guard lock(mutex_);
    pollOutcomes_.push_back(std::move(outcome));
  }

  void blockRequestsUntilCancelled() {
    std::lock_guard lock(mutex_);
    block_ = true;
  }

  bool waitForCalls(std::size_t count) const {
    std::unique_lock lock(mutex_);
    return callsChanged_.wait_for(lock, 3s,
                                  [&] { return calls_.size() >= count; });
  }

  std::vector<DriverCall> calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }

private:
  ir::DeliveryOutcome perform(bool poll, const ir::IrOutboxEntry &entry,
                              const ir::IrProviderRuntimeConfig &config,
                              std::stop_token token) const {
    std::unique_lock lock(mutex_);
    calls_.push_back({.poll = poll,
                      .userIntent = entry.nextRequestUserIntent,
                      .configuredOrigin = config.serverOrigin,
                      .apiKey = config.apiKey,
                      .remoteJobId = entry.remoteJobId,
                      .remoteOrigin = entry.remoteOrigin});
    callsChanged_.notify_all();
    if (block_) {
      blockChanged_.wait(lock, token,
                         [&] { return token.stop_requested() || !block_; });
      return {.status = ir::DeliveryStatus::Cancelled};
    }
    auto &outcomes = poll ? pollOutcomes_ : submitOutcomes_;
    if (outcomes.empty()) {
      return {.status = ir::DeliveryStatus::Succeeded};
    }
    auto outcome = std::move(outcomes.front());
    outcomes.pop_front();
    return outcome;
  }

  ir::IrDriverCapabilities capabilities_;
  mutable std::mutex mutex_;
  mutable std::condition_variable_any callsChanged_;
  mutable std::condition_variable_any blockChanged_;
  mutable std::vector<DriverCall> calls_;
  mutable std::deque<ir::DeliveryOutcome> submitOutcomes_;
  mutable std::deque<ir::DeliveryOutcome> pollOutcomes_;
  mutable bool block_ = false;
};

class ManualWaiter {
public:
  void wait(std::stop_token token,
            std::optional<std::chrono::steady_clock::time_point> deadline) {
    std::stop_callback stopped(token, [&] { condition_.notify_all(); });
    std::unique_lock lock(mutex_);
    ++entered_;
    deadline_ = deadline;
    condition_.notify_all();
    condition_.wait(lock, [&] { return token.stop_requested() || pending_; });
    pending_ = false;
  }

  void wake() {
    std::lock_guard lock(mutex_);
    pending_ = true;
    condition_.notify_all();
  }

  bool waitForEntries(std::size_t count) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 3s, [&] { return entered_ >= count; });
  }

  std::optional<std::chrono::steady_clock::time_point> deadline() {
    std::lock_guard lock(mutex_);
    return deadline_;
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool pending_ = false;
  std::size_t entered_ = 0;
  std::optional<std::chrono::steady_clock::time_point> deadline_;
};

std::string attemptId(int suffix) {
  char value[37]{};
  std::snprintf(value, sizeof(value), "123e4567-e89b-42d3-a456-426614174%03d",
                suffix);
  return value;
}

ir::IrOutboxDraft draft(int suffix, std::int64_t createdAt) {
  return {
      .providerId = "fake",
      .attemptId = attemptId(suffix),
      .chartMd5 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      .chartSha256 =
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      .payloadJson = R"({"score":123})",
      .rulesetProof =
          {
              .rulesetId = "test-rules",
              .rulesetRevision = 1,
              .validationFingerprint = std::string(64, 'd'),
          },
      .createdAtUnixMillis = createdAt};
}

ir::IrActiveProfileConfig
profile(bool enabled = true, bool autoSubmit = true,
        std::string origin = "https://old.example.test") {
  ir::IrActiveProfileConfig result{.profileId = "profile-a"};
  result.providers["fake"] = {.enabled = enabled,
                              .autoSubmit = autoSubmit,
                              .serverOrigin = std::move(origin)};
  return result;
}

class Harness {
public:
  explicit Harness(
      ir::IrDriverCapabilities capabilities = {.readOnly = false,
                                               .chartRankings = false,
                                               .scoreSubmission = true,
                                               .deferredSubmission = true})
      : repository(temp.path() / "replays.db"),
        driver(std::make_shared<FakeDriver>(capabilities)) {
    expect(repository.EnsureSchema(), "service harness schema initializes");
    std::string diagnostic;
    expect(registry.registerDriver(driver, diagnostic),
           "service harness driver registers");
    ir::IrSubmissionServiceOptions options;
    options.wallNowUnixMillis = [this] { return now.load(); };
    options.monotonicNow = [this] {
      return std::chrono::steady_clock::time_point(
          std::chrono::milliseconds(now.load()));
    };
    options.credentialLookup = [this](std::string_view,
                                      std::string_view providerId) {
      std::string value;
      {
        std::lock_guard lock(credentialsMutex);
        const auto found = credentials.find(providerId);
        value = found == credentials.end() ? std::string{} : found->second;
      }
      std::unique_lock lock(credentialLookupMutex);
      if (credentialLookupBlocked) {
        credentialLookupEntered = true;
        credentialLookupChanged.notify_all();
        credentialLookupChanged.wait(
            lock, [&] { return credentialLookupReleased; });
        credentialLookupBlocked = false;
      }
      return value;
    };
    options.waitUntil =
        [this](std::stop_token token,
               std::optional<std::chrono::steady_clock::time_point> deadline) {
          waiter.wait(token, deadline);
        };
    options.wake = [this] { waiter.wake(); };
    options.submissionSucceeded = [this](std::string_view profileId,
                                         std::string_view providerId,
                                         std::string_view origin,
                                         std::string_view md5,
                                         std::string_view sha256) {
      std::lock_guard lock(successMutex);
      successes.push_back(std::string(profileId) + "|" +
                          std::string(providerId) + "|" + std::string(origin) +
                          "|" + std::string(md5) + "|" + std::string(sha256));
    };
    options.credentialChanged = [this](std::string_view,
                                       std::string_view providerId) {
      std::lock_guard lock(successMutex);
      credentialChanges.emplace_back(providerId);
    };
    service = std::make_unique<ir::IrSubmissionService>(
        repository, registry, http, std::move(options));
  }

  ~Harness() {
    if (service) {
      service->stop();
    }
    repository.Shutdown();
  }

  void setCredential(std::string value) {
    std::lock_guard lock(credentialsMutex);
    if (value.empty()) {
      credentials.erase("fake");
    } else {
      credentials["fake"] = std::move(value);
    }
  }

  void blockNextCredentialLookup() {
    std::lock_guard lock(credentialLookupMutex);
    credentialLookupBlocked = true;
    credentialLookupEntered = false;
    credentialLookupReleased = false;
  }

  bool waitForCredentialLookup() {
    std::unique_lock lock(credentialLookupMutex);
    return credentialLookupChanged.wait_for(
        lock, 3s, [&] { return credentialLookupEntered; });
  }

  void releaseCredentialLookup() {
    std::lock_guard lock(credentialLookupMutex);
    credentialLookupReleased = true;
    credentialLookupChanged.notify_all();
  }

  [[nodiscard]] std::size_t credentialChangeCount() const {
    std::lock_guard lock(successMutex);
    return credentialChanges.size();
  }

  bool waitForState(std::string_view attempt,
                    ir::IrOutboxState expected) const {
    return waitForSnapshot(attempt, [&](const auto &snapshot) {
      return snapshot.state == expected;
    });
  }

  bool
  waitForSnapshot(std::string_view attempt,
                  const std::function<bool(const ir::IrAttemptStatusSnapshot &)>
                      &predicate) const {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshot = service->status("fake", attempt);
      if (snapshot.found && predicate(snapshot)) {
        return true;
      }
      std::this_thread::yield();
    }
    return false;
  }

  std::vector<std::string> succeededCallbacks() const {
    std::lock_guard lock(successMutex);
    return successes;
  }

  TemporaryDirectory temp;
  ReplayRepository repository;
  ir::IrDriverRegistry registry;
  std::shared_ptr<FakeDriver> driver;
  FakeHttpClient http;
  std::atomic<std::int64_t> now{1'000'000'000'000LL};
  ManualWaiter waiter;
  mutable std::mutex credentialsMutex;
  std::map<std::string, std::string, std::less<>> credentials;
  std::mutex credentialLookupMutex;
  std::condition_variable credentialLookupChanged;
  bool credentialLookupBlocked = false;
  bool credentialLookupEntered = false;
  bool credentialLookupReleased = false;
  mutable std::mutex successMutex;
  std::vector<std::string> successes;
  std::vector<std::string> credentialChanges;
  std::unique_ptr<ir::IrSubmissionService> service;
};

ir::IrOutboxEntry load(Harness &harness, int suffix) {
  const auto result =
      harness.repository.LoadIrOutbox("fake", attemptId(suffix));
  expect(result.status == ir::IrOutboxReadStatus::Found && result.entry,
         "expected outbox row exists");
  return result.entry.value_or(ir::IrOutboxEntry{});
}

void makeFailed(Harness &harness, int suffix) {
  const auto inserted = harness.repository.EnqueueReadyIrOutboxDraft(
      draft(suffix, harness.now.load()), false);
  expect(inserted.entry.has_value(), "failed fixture inserts");
  if (!inserted.entry) {
    return;
  }
  expect(harness.repository
                 .ClaimIrOutbox(inserted.entry->id, ir::IrOutboxState::Pending,
                                harness.now.load())
                 .status == ir::IrOutboxClaimStatus::Claimed,
         "failed fixture claims");
  expect(harness.repository
                 .ApplyIrOutboxDelivery({
                     .rowId = inserted.entry->id,
                     .nextState = ir::IrOutboxState::FailedPermanent,
                     .lastErrorCode = "rejected",
                     .lastErrorMessage = "rejected",
                     .updatedAtUnixMillis = harness.now.load(),
                 })
                 .status == ir::IrOutboxMutationStatus::Updated,
         "failed fixture stores failure");
}

std::int64_t makeAwaiting(Harness &harness, int suffix,
                          std::string origin = "https://old.example.test") {
  const auto inserted = harness.repository.EnqueueReadyIrOutboxDraft(
      draft(suffix, harness.now.load()), false);
  expect(inserted.entry.has_value(), "deferred fixture inserts");
  if (!inserted.entry) {
    return 0;
  }
  expect(harness.repository
                 .ClaimIrOutbox(inserted.entry->id, ir::IrOutboxState::Pending,
                                harness.now.load())
                 .status == ir::IrOutboxClaimStatus::Claimed,
         "deferred fixture claims");
  expect(harness.repository
                 .ApplyIrOutboxDelivery({
                     .rowId = inserted.entry->id,
                     .nextState = ir::IrOutboxState::AwaitingRemoteResult,
                     .nextAttemptAtUnixMillis = harness.now.load() + 10'000,
                     .remoteJobId = "job-123",
                     .remoteOrigin = std::move(origin),
                     .updatedAtUnixMillis = harness.now.load(),
                 })
                 .status == ir::IrOutboxMutationStatus::Updated,
         "deferred fixture stores job");
  return inserted.entry->id;
}

void testActiveRequestSnapshotsDistinguishSubmitAndPoll() {
  Harness submit;
  submit.setCredential("key");
  submit.repository.EnqueueReadyIrOutboxDraft(draft(20, submit.now.load()),
                                              false);
  submit.driver->blockRequestsUntilCancelled();
  submit.service->start(profile(true));
  expect(submit.driver->waitForCalls(1), "blocked submit starts");
  expect(submit.waitForSnapshot(
             attemptId(20), [](const auto &snapshot) {
               return snapshot.state == ir::IrOutboxState::Uploading &&
                      snapshot.activeRequest ==
                          ir::IrActiveRequestKind::Submit;
             }),
         "fresh claim publishes submit activity");
  submit.service->pauseAndCancel();

  Harness poll;
  poll.setCredential("key");
  makeAwaiting(poll, 21);
  poll.now += 10'000;
  poll.driver->blockRequestsUntilCancelled();
  poll.service->start(profile(true));
  expect(poll.driver->waitForCalls(1), "blocked poll starts");
  expect(poll.waitForSnapshot(
             attemptId(21), [](const auto &snapshot) {
               return snapshot.state == ir::IrOutboxState::Uploading &&
                      snapshot.activeRequest ==
                          ir::IrActiveRequestKind::Poll;
             }),
         "deferred claim publishes poll activity");
  poll.service->pauseAndCancel();
}

void testStartupRecovery() {
  Harness harness;
  const auto pending = harness.repository.EnqueueReadyIrOutboxDraft(
      draft(1, harness.now.load()), false);
  expect(pending.entry.has_value(), "stale pending fixture inserts");
  if (pending.entry) {
    harness.repository.ClaimIrOutbox(
        pending.entry->id, ir::IrOutboxState::Pending, harness.now.load());
  }
  const std::int64_t deferredId = makeAwaiting(harness, 2);
  harness.now += 10'000;
  harness.repository.ClaimIrOutbox(
      deferredId, ir::IrOutboxState::AwaitingRemoteResult, harness.now.load());

  harness.service->start(profile(false));
  expect(load(harness, 1).state == ir::IrOutboxState::Pending,
         "startup recovers stale POST claim to pending");
  const auto recoveredDeferred = load(harness, 2);
  expect(recoveredDeferred.state == ir::IrOutboxState::AwaitingRemoteResult &&
             recoveredDeferred.remoteJobId == "job-123",
         "startup recovers stale poll claim with remote identity");
}

void testDisabledAndReadOnlyProvidersStayPaused() {
  Harness disabled;
  disabled.setCredential("key");
  disabled.repository.EnqueueReadyIrOutboxDraft(draft(3, disabled.now.load()),
                                                false);
  disabled.service->start(profile(false, true));
  disabled.waiter.waitForEntries(2);
  expect(disabled.driver->calls().empty(),
         "disabled provider does not process existing work");

  disabled.service->activateProfile(profile(true, false));
  expect(disabled.driver->waitForCalls(1),
         "re-enabled provider resumes existing work");
  expect(disabled.waitForState(attemptId(3), ir::IrOutboxState::Succeeded),
         "auto-submit disablement does not delete or pause existing rows");

  Harness readOnly({.readOnly = true,
                    .chartRankings = true,
                    .scoreSubmission = false,
                    .deferredSubmission = false});
  readOnly.setCredential("key");
  readOnly.repository.EnqueueReadyIrOutboxDraft(draft(4, readOnly.now.load()),
                                                false);
  readOnly.service->start(profile(true));
  readOnly.waiter.waitForEntries(2);
  expect(readOnly.driver->calls().empty() &&
             load(readOnly, 4).state == ir::IrOutboxState::Pending,
         "read-only provider is excluded from submission work");
}

void testFutureWakeIgnoresBoundedSkippedProviderRows() {
  Harness harness;
  harness.setCredential("key");
  bool skippedRowsInserted = true;
  for (int suffix = 100; suffix < 164; ++suffix) {
    auto skipped = draft(suffix, harness.now.load());
    skipped.providerId = "skipped";
    skippedRowsInserted =
        harness.repository.EnqueueReadyIrOutboxDraft(skipped, false).entry
            .has_value() &&
        skippedRowsInserted;
  }
  expect(skippedRowsInserted, "skipped-provider starvation fixtures insert");
  makeAwaiting(harness, 164);

  const auto expectedDeadline = std::chrono::steady_clock::time_point(
      std::chrono::milliseconds(harness.now.load() + 10'000));
  harness.service->start(profile(true));
  expect(harness.waiter.waitForEntries(2),
         "worker settles after scanning skipped-provider rows");
  expect(harness.waiter.deadline() == expectedDeadline,
         "worker waits for the later enabled-provider attempt");

  harness.now += 10'000;
  harness.service->notifyOutboxChanged();
  expect(harness.driver->waitForCalls(1),
         "enabled-provider attempt runs when its deadline becomes due");
  expect(harness.waitForState(attemptId(164), ir::IrOutboxState::Succeeded),
         "due enabled-provider attempt succeeds despite skipped rows");
}

void testMissingKeyPreservesManualIntentAndReplacementWakes() {
  Harness harness;
  const auto inserted = harness.repository.EnqueueReadyIrOutboxDraft(
      draft(5, harness.now.load()), true);
  expect(inserted.entry.has_value(), "manual missing-key fixture inserts");
  harness.service->start(profile(true));
  expect(harness.waitForState(attemptId(5),
                              ir::IrOutboxState::BlockedConfiguration),
         "missing key blocks the row");
  auto blocked = load(harness, 5);
  expect(blocked.nextRequestUserIntent && blocked.requestAttemptCount == 0,
         "missing key blocks without claiming or consuming manual intent");
  expect(harness.credentialChangeCount() == 0,
         "initial credential observation is not reported as a replacement");

  harness.setCredential("replacement-key");
  harness.service->notifyConfigurationChanged();
  expect(harness.driver->waitForCalls(1),
         "credential replacement unblocks and wakes the worker");
  const auto calls = harness.driver->calls();
  expect(calls.front().userIntent && calls.front().apiKey == "replacement-key",
         "unblocked POST consumes manual intent with the current key");
  expect(harness.waitForState(attemptId(5), ir::IrOutboxState::Succeeded),
         "unblocked row succeeds");
  expect(harness.credentialChangeCount() == 1,
         "credential replacement publishes invalidation callback");
}

void testProviderRuntimeChangeUnblocksRows() {
  Harness harness;
  harness.setCredential("key");
  harness.repository.EnqueueReadyIrOutboxDraft(draft(23, harness.now.load()),
                                               false);
  harness.driver->pushSubmit({
      .status = ir::DeliveryStatus::BlockedConfiguration,
      .code = "authentication_required",
      .diagnostic = "old origin rejected the credential",
  });
  harness.service->start(profile(true, true, "https://old.example.test"));
  expect(harness.waitForState(attemptId(23),
                              ir::IrOutboxState::BlockedConfiguration),
         "provider runtime fixture reaches blocked configuration");

  harness.service->activateProfile(
      profile(true, true, "https://new.example.test"));
  expect(harness.driver->waitForCalls(2),
         "provider runtime change retries a blocked row");
  const auto calls = harness.driver->calls();
  expect(calls.size() >= 2 &&
             calls.back().configuredOrigin == "https://new.example.test",
         "retried submission uses the changed provider origin");
  expect(harness.waitForState(attemptId(23), ir::IrOutboxState::Succeeded),
         "provider runtime retry can complete without a credential change");
}

void testManualEnqueueRequiresFreshRulesetProof() {
  Harness harness;
  harness.service->start(profile(true));
  auto missingProof = draft(19, harness.now.load());
  missingProof.rulesetProof = {};
  const auto rejected = harness.service->enqueueManual(missingProof);
  expect(rejected.status == ir::IrOutboxInsertStatus::Invalid,
         "manual enqueue rejects a missing ruleset proof");
  expect(harness.repository.LoadIrOutbox("fake", missingProof.attemptId)
             .status == ir::IrOutboxReadStatus::NotFound,
         "invalid manual proof creates no durable row");
}

void testAutomaticAndManualRequestsUseCurrentOrigin() {
  Harness harness;
  harness.setCredential("current-key");
  harness.repository.EnqueueReadyIrOutboxDraft(draft(6, harness.now.load()),
                                               false);
  harness.repository.EnqueueReadyIrOutboxDraft(draft(7, harness.now.load() + 1),
                                               true);
  harness.repository.EnqueueReadyIrOutboxDraft(
      draft(18, harness.now.load() + 2), false);
  harness.service->start(profile(true, true, "https://new.example.test"));
  expect(harness.driver->waitForCalls(3),
         "the worker drains every due automatic and manual row");
  const auto calls = harness.driver->calls();
  expect(calls.size() >= 3 && !calls[0].userIntent && calls[1].userIntent &&
             !calls[2].userIntent,
         "automatic POST omits intent while manual POST carries it");
  expect(calls.size() >= 3 &&
             calls[0].configuredOrigin == "https://new.example.test" &&
             calls[1].configuredOrigin == "https://new.example.test" &&
             calls[2].configuredOrigin == "https://new.example.test",
         "pending rows use the origin configured when POST begins");
}

void testDeferredPollingPinsOriginAndNeverReposts() {
  Harness harness;
  harness.setCredential("key");
  const auto inserted = harness.repository.EnqueueReadyIrOutboxDraft(
      draft(8, harness.now.load()), false);
  expect(inserted.entry.has_value(), "deferred flow fixture inserts");
  harness.driver->pushSubmit({
      .status = ir::DeliveryStatus::Deferred,
      .remoteJobId = "job-remote",
      .remoteOrigin = "https://old.example.test",
  });
  harness.driver->pushPoll({.status = ir::DeliveryStatus::Ongoing});
  harness.driver->pushPoll({.status = ir::DeliveryStatus::Succeeded});
  harness.service->start(profile(true, true, "https://old.example.test"));
  expect(harness.waitForState(attemptId(8),
                              ir::IrOutboxState::AwaitingRemoteResult),
         "202 stores awaiting state");
  auto awaiting = load(harness, 8);
  expect(awaiting.remoteJobId == "job-remote" &&
             awaiting.remoteOrigin == "https://old.example.test" &&
             awaiting.nextAttemptAtUnixMillis == harness.now.load() + 10'000,
         "202 persists job, request origin, and poll delay");

  harness.service->activateProfile(
      profile(true, true, "https://changed.example.test"));
  expect(harness.service->retry(awaiting.id).status ==
             ir::IrOutboxMutationStatus::Updated,
         "manual retry makes deferred row due without clearing it");
  expect(harness.driver->waitForCalls(2),
         "manual retry of deferred row polls immediately");
  auto calls = harness.driver->calls();
  expect(!calls[1].userIntent && calls[1].poll &&
             calls[1].remoteJobId == "job-remote" &&
             calls[1].remoteOrigin == "https://old.example.test" &&
             calls[1].configuredOrigin == "https://changed.example.test",
         "poll retains original origin and never carries user intent");
  expect(harness.waitForSnapshot(
             attemptId(8),
             [](const auto &snapshot) {
               return snapshot.state ==
                          ir::IrOutboxState::AwaitingRemoteResult &&
                      snapshot.requestAttemptCount >= 2;
             }),
         "ongoing poll remains awaiting");

  awaiting = load(harness, 8);
  expect(harness.service->retry(awaiting.id).status ==
             ir::IrOutboxMutationStatus::Updated,
         "second deferred retry is accepted");
  expect(harness.driver->waitForCalls(3), "second deferred retry polls");
  calls = harness.driver->calls();
  expect(!calls[0].poll && calls[1].poll && calls[2].poll,
         "awaiting row is POSTed once and polled thereafter");
  expect(harness.waitForState(attemptId(8), ir::IrOutboxState::Succeeded),
         "completed poll succeeds");
  const auto callbacks = harness.succeededCallbacks();
  expect(callbacks.size() == 1 &&
             callbacks.front().find("https://old.example.test") !=
                 std::string::npos,
         "completion invalidates ranking cache at persisted origin");
}

void testPersistedBackoffAndRetryAfter() {
  Harness harness;
  harness.setCredential("key");
  harness.repository.EnqueueReadyIrOutboxDraft(draft(9, harness.now.load()),
                                               false);
  for (int index = 0; index < 5; ++index) {
    harness.driver->pushSubmit({.status = ir::DeliveryStatus::TransientFailure,
                                .code = "offline",
                                .diagnostic = "offline"});
  }
  harness.service->start(profile(true));
  const std::vector<std::int64_t> delays{10'000, 30'000, 120'000, 600'000,
                                         3'600'000};
  for (std::size_t index = 0; index < delays.size(); ++index) {
    expect(harness.driver->waitForCalls(index + 1),
           "transient attempt reaches fake driver");
    expect(harness.waitForSnapshot(attemptId(9),
                                   [&](const auto &snapshot) {
                                     return snapshot.state ==
                                                ir::IrOutboxState::Pending &&
                                            snapshot.consecutiveFailureCount ==
                                                static_cast<int>(index + 1);
                                   }),
           "transient result is persisted before inspection");
    const auto expectedTime = harness.now.load() + delays[index];
    const auto pending = load(harness, 9);
    expect(pending.state == ir::IrOutboxState::Pending &&
               pending.consecutiveFailureCount == static_cast<int>(index + 1) &&
               pending.nextAttemptAtUnixMillis == expectedTime,
           "transient failures persist the capped backoff schedule");
    harness.now = expectedTime;
    harness.service->notifyOutboxChanged();
  }

  Harness retryAfter;
  retryAfter.setCredential("key");
  retryAfter.repository.EnqueueReadyIrOutboxDraft(
      draft(10, retryAfter.now.load()), false);
  retryAfter.driver->pushSubmit({
      .status = ir::DeliveryStatus::TransientFailure,
      .retryAfterDelay = std::chrono::seconds(120),
  });
  retryAfter.service->start(profile(true));
  expect(retryAfter.driver->waitForCalls(1),
         "Retry-After fixture reaches driver");
  expect(retryAfter.waitForSnapshot(attemptId(10),
                                    [](const auto &snapshot) {
                                      return snapshot.state ==
                                                 ir::IrOutboxState::Pending &&
                                             snapshot.consecutiveFailureCount ==
                                                 1;
                                    }),
         "Retry-After result is persisted before inspection");
  expect(load(retryAfter, 10).nextAttemptAtUnixMillis ==
             retryAfter.now.load() + 120'000,
         "longer Retry-After extends persisted backoff");
}

void testPermanentFailureRetryAllAndDeferredPreservation() {
  Harness harness;
  makeFailed(harness, 11);
  makeFailed(harness, 12);
  const auto deferredId = makeAwaiting(harness, 13);
  const auto pending = harness.repository.EnqueueReadyIrOutboxDraft(
      draft(22, harness.now.load() + 60'000), false);
  expect(pending.entry.has_value(), "delayed pending fixture inserts");
  harness.service->start(profile(false));
  const auto retried = harness.service->retryAll("fake");
  expect(retried.status == ir::IrOutboxMutationStatus::Updated &&
             retried.affectedRows == 4,
         "Retry All resets pending and failed rows and schedules deferred poll");
  expect(load(harness, 11).state == ir::IrOutboxState::Pending &&
             load(harness, 11).nextRequestUserIntent &&
             load(harness, 12).state == ir::IrOutboxState::Pending &&
             load(harness, 12).nextRequestUserIntent,
         "Retry All marks new POSTs as explicit user intent");
  const auto retriedPending = load(harness, 22);
  expect(retriedPending.state == ir::IrOutboxState::Pending &&
             retriedPending.nextAttemptAtUnixMillis == harness.now.load() &&
             retriedPending.nextRequestUserIntent,
         "Retry All makes delayed pending work due with user intent");
  const auto deferred = load(harness, 13);
  expect(deferred.id == deferredId &&
             deferred.state == ir::IrOutboxState::AwaitingRemoteResult &&
             deferred.remoteJobId == "job-123" &&
             !deferred.nextRequestUserIntent,
         "Retry All preserves deferred remote identity without POST intent");

  Harness permanent;
  permanent.setCredential("secret-token");
  permanent.repository.EnqueueReadyIrOutboxDraft(
      draft(14, permanent.now.load()), false);
  permanent.driver->pushSubmit({
      .status = ir::DeliveryStatus::PermanentFailure,
      .code = "rejected",
      .diagnostic = "provider echoed secret-token",
  });
  permanent.service->start(profile(true));
  expect(
      permanent.waitForState(attemptId(14), ir::IrOutboxState::FailedPermanent),
      "permanent provider failure is terminal until manual retry");
  const auto failed = load(permanent, 14);
  expect(failed.lastErrorCode == "rejected" &&
             failed.lastErrorMessage == "provider echoed [redacted]" &&
             failed.lastErrorMessage.find("secret-token") == std::string::npos,
         "permanent failure is bounded and cannot persist the API key");
  expect(permanent.service->retry(failed.id).status ==
             ir::IrOutboxMutationStatus::Updated,
         "manual retry resets permanent failure");
  expect(permanent.driver->waitForCalls(2),
         "manual permanent retry performs another POST");
  expect(permanent.driver->calls()[1].userIntent,
         "manual permanent retry carries user intent");
}

void testSucceededPurgeAndSnapshotReads() {
  Harness harness;
  const auto old = harness.repository.EnqueueReadyIrOutboxDraft(
      draft(15, harness.now.load() - 9LL * 24 * 60 * 60 * 1000), false);
  const auto recent = harness.repository.EnqueueReadyIrOutboxDraft(
      draft(16, harness.now.load() - 2LL * 24 * 60 * 60 * 1000), false);
  for (const auto *inserted : {&old, &recent}) {
    expect(inserted->entry.has_value(), "purge fixture inserts");
    if (!inserted->entry) {
      continue;
    }
    harness.repository.ClaimIrOutbox(inserted->entry->id,
                                     ir::IrOutboxState::Pending,
                                     inserted->entry->createdAtUnixMillis);
    harness.repository.ApplyIrOutboxDelivery({
        .rowId = inserted->entry->id,
        .nextState = ir::IrOutboxState::Succeeded,
        .updatedAtUnixMillis = inserted->entry->createdAtUnixMillis,
        .completedAtUnixMillis = inserted->entry->createdAtUnixMillis,
    });
  }
  harness.service->start(profile(false));
  expect(harness.repository.LoadIrOutbox("fake", attemptId(15)).status ==
             ir::IrOutboxReadStatus::NotFound,
         "startup purges succeeded rows older than seven days");
  expect(harness.service->status("fake", attemptId(16)).found &&
             harness.service->counts("fake").succeeded == 1,
         "recent success seeds bounded in-memory status and counts");
  harness.repository.Shutdown();
  expect(harness.service->status("fake", attemptId(16)).found &&
             harness.service->counts("fake").succeeded == 1,
         "UI snapshot reads do not access SQLite");
}

void testPauseCancelsInflightAndRecoversClaim() {
  Harness harness;
  harness.setCredential("key");
  harness.repository.EnqueueReadyIrOutboxDraft(draft(17, harness.now.load()),
                                               false);
  harness.driver->blockRequestsUntilCancelled();
  harness.service->start(profile(true));
  expect(harness.driver->waitForCalls(1), "in-flight fixture starts request");
  harness.service->pauseAndCancel();
  const auto recovered = load(harness, 17);
  expect(recovered.state == ir::IrOutboxState::Pending &&
             recovered.requestAttemptCount == 1,
         "profile pause cancels I/O, waits, and recovers the claimed row");
  expect(harness.driver->calls().size() == 1,
         "paused service performs no additional requests");
}

void testForegroundRecoversAbandonedClaim() {
  Harness harness;
  const auto inserted = harness.repository.EnqueueReadyIrOutboxDraft(
      draft(18, harness.now.load()), false);
  expect(inserted.entry.has_value(), "foreground recovery fixture inserts");
  harness.service->start(profile(false));
  harness.service->setApplicationActive(false);
  if (!inserted.entry) {
    return;
  }
  expect(harness.repository
                 .ClaimIrOutbox(inserted.entry->id,
                                ir::IrOutboxState::Pending,
                                harness.now.load())
                 .status == ir::IrOutboxClaimStatus::Claimed,
         "suspended request fixture leaves an uploading claim");

  harness.service->setApplicationActive(true);

  const auto recovered = load(harness, 18);
  const auto snapshot = harness.service->status("fake", attemptId(18));
  expect(recovered.state == ir::IrOutboxState::Pending &&
             recovered.requestAttemptCount == 1 && snapshot.found &&
             snapshot.state == ir::IrOutboxState::Pending,
         "foreground activation recovers and republishes an abandoned claim");
}

void testForegroundPreservesPendingCredentialChange() {
  Harness harness;
  harness.repository.EnqueueReadyIrOutboxDraft(draft(19, harness.now.load()),
                                                false);
  harness.service->start(profile(true));
  expect(harness.waitForState(attemptId(19),
                              ir::IrOutboxState::BlockedConfiguration),
         "missing credential blocks the foreground-change fixture");

  harness.service->setApplicationActive(false);
  harness.setCredential("replacement-key");
  harness.service->notifyConfigurationChanged();
  harness.service->setApplicationActive(true);

  expect(harness.driver->waitForCalls(1) &&
             harness.waitForState(attemptId(19),
                                  ir::IrOutboxState::Succeeded),
         "a credential changed in background unblocks delivery on foreground");
}

void testLifecycleCancellationWinsBeforeRequestStarts() {
  Harness harness;
  harness.setCredential("captured-key");
  harness.service->start(profile(true));
  expect(harness.waiter.waitForEntries(1),
         "pre-request race worker reaches its idle wait");
  harness.driver->blockRequestsUntilCancelled();
  harness.blockNextCredentialLookup();
  harness.repository.EnqueueReadyIrOutboxDraft(draft(22, harness.now.load()),
                                                false);
  harness.service->notifyOutboxChanged();
  expect(harness.waitForCredentialLookup(),
         "pre-request race pauses after capturing the credential");

  harness.setCredential({});
  harness.service->setApplicationActive(false);
  std::atomic_bool foregroundReturned{false};
  std::thread foreground([&] {
    harness.service->setApplicationActive(true);
    foregroundReturned.store(true);
  });
  harness.releaseCredentialLookup();

  bool requestStartedWhileInactive = false;
  bool blockedAfterForeground = false;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    requestStartedWhileInactive = !harness.driver->calls().empty();
    const auto snapshot =
        harness.service->status("fake", attemptId(22));
    blockedAfterForeground =
        foregroundReturned.load() && snapshot.found &&
        snapshot.state == ir::IrOutboxState::BlockedConfiguration;
    if (requestStartedWhileInactive || blockedAfterForeground) {
      break;
    }
    std::this_thread::yield();
  }
  if (requestStartedWhileInactive || !foregroundReturned.load()) {
    harness.service->stop();
  }
  foreground.join();

  expect(!requestStartedWhileInactive && foregroundReturned.load() &&
             blockedAfterForeground,
         "lifecycle cancellation prevents a fresh request before foreground");
}

} // namespace

int main() {
  static_assert(ir::kMaximumAttemptStatusSnapshots > 0);
  testActiveRequestSnapshotsDistinguishSubmitAndPoll();
  testStartupRecovery();
  testDisabledAndReadOnlyProvidersStayPaused();
  testFutureWakeIgnoresBoundedSkippedProviderRows();
  testMissingKeyPreservesManualIntentAndReplacementWakes();
  testProviderRuntimeChangeUnblocksRows();
  testManualEnqueueRequiresFreshRulesetProof();
  testAutomaticAndManualRequestsUseCurrentOrigin();
  testDeferredPollingPinsOriginAndNeverReposts();
  testPersistedBackoffAndRetryAfter();
  testPermanentFailureRetryAllAndDeferredPreservation();
  testSucceededPurgeAndSnapshotReads();
  testPauseCancelsInflightAndRecoversClaim();
  testForegroundRecoversAbandonedClaim();
  testForegroundPreservesPendingCredentialChange();
  testLifecycleCancellationWinsBeforeRequestStarts();

  if (failures != 0) {
    std::cerr << failures << " IR submission service test(s) failed\n";
    return 1;
  }
  std::cout << "IR submission service tests passed\n";
  return 0;
}
