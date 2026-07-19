#include "ir/IrSubmissionService.h"

#include "FileChecksum.h"
#include "Utils.h"
#include "ir/IrHttpClient.h"
#include "ir/IrSettingsPresentation.h"
#include "ir/tachi/TachiDriver.h"
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

#include <sqlite3.h>

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

class TachiSuccessHttpClient final : public ir::IrHttpClient {
public:
  ir::IrHttpResponse perform(const ir::IrHttpRequest &request,
                             std::stop_token) noexcept override {
    std::lock_guard lock(mutex_);
    requests_.push_back(request);
    changed_.notify_all();
    return {
        .statusCode = 200,
        .body = R"({"success":true,"description":"Import successful.","body":{"scoreIDs":[],"errors":[]}})",
    };
  }

  bool waitForRequests(std::size_t count) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 3s,
                             [&] { return requests_.size() >= count; });
  }

  std::vector<ir::IrHttpRequest> requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<ir::IrHttpRequest> requests_;
};

struct DriverCall {
  bool poll = false;
  bool userIntent = false;
  std::vector<std::int64_t> rowIds;
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
    ++capabilitiesCalls_;
    return capabilities_;
  }

  [[nodiscard]] int capabilitiesCalls() const noexcept {
    return capabilitiesCalls_.load();
  }

  ir::IrOutboxBatchPlan
  planBatch(std::span<const ir::IrOutboxEntry> due) const override {
    if (due.empty()) {
      return {.status = ir::IrOutboxBatchPlanStatus::Invalid,
              .diagnostic = "fake batch has no rows"};
    }
    {
      std::lock_guard lock(mutex_);
      if (!rejectedPlanAttemptId_.empty() &&
          due.front().attemptId == rejectedPlanAttemptId_ &&
          (!rejectedPlanRowId_ ||
           std::ranges::any_of(due, [&](const auto &entry) {
             return entry.id == *rejectedPlanRowId_;
           }))) {
        return {.status = rejectedPlanStatus_,
                .rejectedRowId = identifyRejectedPlan_
                                     ? std::optional(rejectedPlanRowId_.value_or(
                                           due.front().id))
                                     : std::nullopt,
                .diagnostic = "fake rejected the first due row"};
      }
    }
    const auto &first = due.front();
    std::vector<std::int64_t> rowIds;
    for (const auto &entry : due) {
      if (entry.state != first.state) {
        continue;
      }
      if (entry.state == ir::IrOutboxState::AwaitingRemoteResult &&
          (entry.remoteJobId != first.remoteJobId ||
           entry.remoteOrigin != first.remoteOrigin)) {
        continue;
      }
      rowIds.push_back(entry.id);
    }
    return {.status = ir::IrOutboxBatchPlanStatus::Planned,
            .rowIds = std::move(rowIds)};
  }

  ir::DeliveryOutcome submit(const ir::IrOutboxEntry &entry,
                             const ir::IrProviderRuntimeConfig &config,
                             ir::IrHttpClient &,
                             std::stop_token token) const override {
    return perform(false, std::span<const ir::IrOutboxEntry>(&entry, 1),
                   entry.nextRequestUserIntent, config, token);
  }

  ir::DeliveryOutcome submitBatch(
      std::span<const ir::IrOutboxEntry> entries, bool userIntent,
      const ir::IrProviderRuntimeConfig &config, ir::IrHttpClient &,
      std::stop_token token) const override {
    return perform(false, entries, userIntent, config, token);
  }

  ir::DeliveryOutcome poll(const ir::IrOutboxEntry &entry,
                           const ir::IrProviderRuntimeConfig &config,
                           ir::IrHttpClient &,
                           std::stop_token token) const override {
    return perform(true, std::span<const ir::IrOutboxEntry>(&entry, 1), false,
                   config, token);
  }

  ir::DeliveryOutcome pollBatch(
      std::span<const ir::IrOutboxEntry> entries,
      const ir::IrProviderRuntimeConfig &config, ir::IrHttpClient &,
      std::stop_token token) const override {
    return perform(true, entries, false, config, token);
  }

  ir::IrUserScoreSnapshotOutcome fetchUserScoreSnapshot(
      const ir::IrProviderRuntimeConfig &config, ir::IrHttpClient &,
      std::stop_token token, ir::IrUserScoreProgress progress) const override {
    std::stop_callback stopped(token,
                               [this] { reconciliationChanged_.notify_all(); });
    std::unique_lock lock(mutex_);
    activeReconciliationToken_ = token;
    ++reconciliationCalls_;
    reconciliationApiKeys_.push_back(config.apiKey);
    reconciliationChanged_.notify_all();
    lock.unlock();

    progress("bms-7k", 0, 2);
    lock.lock();
    reconciliationStage_ = 1;
    reconciliationChanged_.notify_all();
    if (ignoreReconciliationCancellation_) {
      reconciliationChanged_.wait(
          lock, [&] { return reconciliationReleaseStage_ >= 1; });
    } else {
      reconciliationChanged_.wait(lock, token, [&] {
        return token.stop_requested() || reconciliationReleaseStage_ >= 1;
      });
    }
    if (token.stop_requested() && !ignoreReconciliationCancellation_) {
      return {.status = ir::IrUserScoreSnapshotStatus::Cancelled,
              .code = "cancelled",
              .diagnostic = "fake reconciliation was cancelled"};
    }
    lock.unlock();

    progress("bms-7k", 1, 2);
    progress("bms-14k", 1, 2);
    lock.lock();
    reconciliationStage_ = 2;
    reconciliationChanged_.notify_all();
    if (ignoreReconciliationCancellation_) {
      reconciliationChanged_.wait(
          lock, [&] { return reconciliationReleaseStage_ >= 2; });
    } else {
      reconciliationChanged_.wait(lock, token, [&] {
        return token.stop_requested() || reconciliationReleaseStage_ >= 2;
      });
    }
    if (token.stop_requested() && !ignoreReconciliationCancellation_) {
      return {.status = ir::IrUserScoreSnapshotStatus::Cancelled,
              .code = "cancelled",
              .diagnostic = "fake reconciliation was cancelled"};
    }
    lock.unlock();

    progress("bms-14k", 2, 2);
    lock.lock();
    if (reconciliationOutcomes_.empty()) {
      return {.status = ir::IrUserScoreSnapshotStatus::Succeeded,
              .snapshot = ir::IrUserScoreSnapshot{}};
    }
    auto outcome = std::move(reconciliationOutcomes_.front());
    reconciliationOutcomes_.pop_front();
    return outcome;
  }

  void pushSubmit(ir::DeliveryOutcome outcome) {
    std::lock_guard lock(mutex_);
    submitOutcomes_.push_back(std::move(outcome));
  }

  void pushPoll(ir::DeliveryOutcome outcome) {
    std::lock_guard lock(mutex_);
    pollOutcomes_.push_back(std::move(outcome));
  }

  void pushReconciliation(ir::IrUserScoreSnapshotOutcome outcome) {
    std::lock_guard lock(mutex_);
    reconciliationOutcomes_.push_back(std::move(outcome));
  }

  void rejectPlanForAttempt(std::string attemptId,
                            ir::IrOutboxBatchPlanStatus status,
                            bool identifyRejectedRow = true,
                            std::optional<std::int64_t> rejectedRowId =
                                std::nullopt) {
    std::lock_guard lock(mutex_);
    rejectedPlanAttemptId_ = std::move(attemptId);
    rejectedPlanStatus_ = status;
    identifyRejectedPlan_ = identifyRejectedRow;
    rejectedPlanRowId_ = rejectedRowId;
  }

  void blockRequestsUntilCancelled() {
    std::lock_guard lock(mutex_);
    block_ = true;
  }

  void blockRequestsUntilReleased() {
    std::lock_guard lock(mutex_);
    blockUntilReleased_ = true;
  }

  void releaseBlockedRequests() {
    std::lock_guard lock(mutex_);
    blockUntilReleased_ = false;
    blockChanged_.notify_all();
  }

  bool waitForCalls(std::size_t count) const {
    std::unique_lock lock(mutex_);
    return callsChanged_.wait_for(lock, 3s,
                                  [&] { return calls_.size() >= count; });
  }

  bool waitForReconciliationStage(int stage) const {
    std::unique_lock lock(mutex_);
    return reconciliationChanged_.wait_for(
        lock, 3s, [&] { return reconciliationStage_ >= stage; });
  }

  bool waitForReconciliationCalls(std::size_t count) const {
    std::unique_lock lock(mutex_);
    return reconciliationChanged_.wait_for(
        lock, 3s, [&] { return reconciliationCalls_ >= count; });
  }

  bool waitForReconciliationCancellation() const {
    std::unique_lock lock(mutex_);
    return reconciliationChanged_.wait_for(lock, 3s, [&] {
      return activeReconciliationToken_ &&
             activeReconciliationToken_->stop_requested();
    });
  }

  void releaseReconciliationStage(int stage) {
    std::lock_guard lock(mutex_);
    reconciliationReleaseStage_ =
        std::max(reconciliationReleaseStage_, stage);
    reconciliationChanged_.notify_all();
  }

  void ignoreReconciliationCancellation() {
    std::lock_guard lock(mutex_);
    ignoreReconciliationCancellation_ = true;
  }

  std::size_t reconciliationCalls() const {
    std::lock_guard lock(mutex_);
    return reconciliationCalls_;
  }

  std::vector<DriverCall> calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }

private:
  mutable std::atomic_int capabilitiesCalls_{0};
  ir::DeliveryOutcome perform(bool poll,
                              std::span<const ir::IrOutboxEntry> entries,
                              bool userIntent,
                              const ir::IrProviderRuntimeConfig &config,
                              std::stop_token token) const {
    std::unique_lock lock(mutex_);
    std::vector<std::int64_t> rowIds;
    rowIds.reserve(entries.size());
    for (const auto &entry : entries) {
      rowIds.push_back(entry.id);
    }
    const ir::IrOutboxEntry empty;
    const auto &first = entries.empty() ? empty : entries.front();
    calls_.push_back({.poll = poll,
                      .userIntent = userIntent,
                      .rowIds = std::move(rowIds),
                      .configuredOrigin = config.serverOrigin,
                      .apiKey = config.apiKey,
                      .remoteJobId = first.remoteJobId,
                      .remoteOrigin = first.remoteOrigin});
    callsChanged_.notify_all();
    if (block_) {
      blockChanged_.wait(lock, token,
                         [&] { return token.stop_requested() || !block_; });
      return {.status = ir::DeliveryStatus::Cancelled};
    }
    if (blockUntilReleased_) {
      blockChanged_.wait(lock, [&] { return !blockUntilReleased_; });
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
  mutable std::condition_variable_any reconciliationChanged_;
  mutable std::vector<DriverCall> calls_;
  mutable std::deque<ir::DeliveryOutcome> submitOutcomes_;
  mutable std::deque<ir::DeliveryOutcome> pollOutcomes_;
  mutable std::deque<ir::IrUserScoreSnapshotOutcome> reconciliationOutcomes_;
  mutable std::vector<std::string> reconciliationApiKeys_;
  mutable std::string rejectedPlanAttemptId_;
  mutable ir::IrOutboxBatchPlanStatus rejectedPlanStatus_ =
      ir::IrOutboxBatchPlanStatus::Invalid;
  mutable bool identifyRejectedPlan_ = true;
  mutable std::optional<std::int64_t> rejectedPlanRowId_;
  mutable std::optional<std::stop_token> activeReconciliationToken_;
  mutable std::size_t reconciliationCalls_ = 0;
  mutable int reconciliationStage_ = 0;
  mutable int reconciliationReleaseStage_ = 0;
  mutable bool ignoreReconciliationCancellation_ = false;
  mutable bool block_ = false;
  mutable bool blockUntilReleased_ = false;
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

  std::size_t entryCount() {
    std::lock_guard lock(mutex_);
    return entered_;
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

ir::IrOutboxDraft tachiDraft(int suffix, std::int64_t createdAt) {
  ir::IrOutboxDraft result{
      .providerId = "tachi",
      .attemptId = attemptId(suffix),
      .chartMd5 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      .chartSha256 =
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      .payloadJson =
          R"({"meta":{"game":"bms","playtype":"7K","service":"AsoBMaShow"},"scores":[{"score":123}]})",
      .createdAtUnixMillis = createdAt,
  };
  const std::string proofInput =
      "tachi-lr2-proof-v1\n3:lr2\n3\n" +
      std::to_string(result.attemptId.size()) + ":" + result.attemptId + "\n" +
      std::to_string(result.chartSha256.size()) + ":" + result.chartSha256 +
      "\n" + std::to_string(result.payloadJson.size()) + ":" +
      result.payloadJson;
  result.rulesetProof = {
      .rulesetId = "lr2",
      .rulesetRevision = 3,
      .validationFingerprint = file_checksum::sha256(proofInput),
  };
  return result;
}

ir::IrRemoteScore remoteScore(std::string id = "remote-score-1") {
  return {
      .remoteUserId = 42,
      .game = "bms-7k",
      .remoteScoreId = std::move(id),
      .remoteChartId = "remote-chart-1",
      .chartMd5 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      .chartSha256 =
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      .title = "IR service fixture",
      .artist = "Test",
      .service = "Fake IR",
      .noteCount = 50,
      .score = 91,
      .lampRank = kClearTypeHardClearRank,
      .timeAddedUnixMillis = 1'000'000'000'000LL,
  };
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

result_persistence::ChartResultAttempt
canonicalAttempt(const ir::IrOutboxDraft &outboxDraft,
                 const std::filesystem::path &root) {
  ReplayData replay;
  replay.chartMeta.BmsPath =
      root / "BMS" / (outboxDraft.attemptId + ".bms");
  replay.chartMeta.MD5 = outboxDraft.chartMd5;
  replay.chartMeta.SHA256 = outboxDraft.chartSha256;
  replay.chartMeta.Title = "IR service fixture";
  replay.chartMeta.Artist = "Test";
  replay.chartMeta.Rank = 2;
  replay.chartMeta.TotalNotes = 50;
  replay.chartMeta.TotalLongNotes = 1;
  replay.chartMeta.LnMode = 2;
  replay.initialGaugeType = GaugeType::Hard;
  replay.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  replay.finalScore = 91;
  replay.maxCombo = 45;
  replay.finalGauge = 82.5f;
  replay.clearType = kClearTypeHardClearRank;
  replay.playOption = "RANDOM";
  replay.playOptionSeed = 1234;
  replay.events.push_back({.action = ReplayEventAction::Press,
                           .lane = 3,
                           .noteTimeMicros = 100'000,
                           .songTimeMicros = 100'100,
                           .judgeTimeMicros = 100'050,
                           .judgement = PGreat,
                           .diffMicros = -50,
                           .gauge = 82.5f,
                           .gaugeType = GaugeType::Hard,
                           .combo = 1,
                           .score = 2});

  ScoreProvenanceBuildInput provenanceInput;
  provenanceInput.chartMeta = replay.chartMeta;
  provenanceInput.longNoteMode = replay.chartMeta.LnMode;
  provenanceInput.judgeRankSource = JudgeRankSource::Chart;
  provenanceInput.sourceJudgeRank = replay.chartMeta.Rank;
  provenanceInput.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  provenanceInput.totalNotes = replay.chartMeta.TotalNotes;
  provenanceInput.effectiveGaugeTotal = 176.0;
  provenanceInput.candidateSelection = gameplay::CandidateSelectionMode::LR2;
  provenanceInput.gaugeType = replay.initialGaugeType;
  provenanceInput.gaugeAutoShift = replay.gaugeAutoShift;
  provenanceInput.player1 = {.option = "RANDOM", .seed = 1234};
  provenanceInput.inputDevices = {InputDeviceCategory::Keyboard};
  provenanceInput.ruleset = RulesetDescriptor::Current();
  replay.provenance = makeScoreProvenance(provenanceInput);
  replay.provenance.eligibility = ScoreEligibility::Verified;

  result_persistence::ChartScoreWrite score{
      .chartPath = Utils::GetStoragePathUtf8RelativeToDocuments(
          replay.chartMeta.BmsPath, "BMS/"),
      .chartMd5 = outboxDraft.chartMd5,
      .chartSha256 = outboxDraft.chartSha256,
      .chartTitle = replay.chartMeta.Title,
      .chartArtist = replay.chartMeta.Artist,
      .longNoteMode = replay.chartMeta.LnMode,
      .score = replay.finalScore,
      .maxScore = replay.chartMeta.TotalNotes * 2,
      .maxCombo = replay.maxCombo,
      .comboBreak = 5,
      .pGreat = 40,
      .great = 11,
      .good = 2,
      .bad = 1,
      .poor = 3,
      .kPoor = 4,
      .fast = 7,
      .slow = 8,
      .finalGauge = replay.finalGauge,
      .clearType = replay.clearType,
      .provenance = replay.provenance,
  };
  return {
      .attemptId = outboxDraft.attemptId,
      .replay = replay,
      .score = score,
      .payloadFingerprint =
          result_persistence::payloadFingerprint(replay, score),
  };
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
    options.wallNowUnixMillis = [this] {
      ++wallNowCalls;
      return now.load();
    };
    options.monotonicNow = [this] {
      if (monotonicHook) {
        monotonicHook();
      }
      if (useIndependentSteady.load()) {
        return std::chrono::steady_clock::time_point(
            std::chrono::milliseconds(steadyNow.load()));
      }
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
    options.wake = [this] {
      if (wakeHook) {
        wakeHook();
      }
      waiter.wake();
    };
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
    options.remoteSnapshotApplied =
        [this](std::string_view profileId, std::string_view providerId,
               std::string_view origin, std::int64_t syncGeneration,
               std::span<const ir::IrRemoteScore> scores,
               std::string &diagnostic) {
          const auto mirror = repository.ListIrRemoteScores(providerId, origin);
          std::lock_guard lock(projectionMutex);
          ++projectionCalls;
          projectedProfile = profileId;
          projectedProvider = providerId;
          projectedOrigin = origin;
          projectedGeneration = syncGeneration;
          projectedScoreIds.clear();
          for (const auto &score : scores) {
            projectedScoreIds.push_back(score.remoteScoreId);
          }
          projectionSawCommittedMirror =
              mirror.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
              mirror.scores.size() == scores.size();
          if (!projectionShouldSucceed) {
            diagnostic = "could not project synchronized scores";
            return false;
          }
          return true;
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

  ir::IrOutboxInsertOutcome enqueueReady(const ir::IrOutboxDraft &value,
                                         bool userIntent) {
    const auto attempt = canonicalAttempt(value, temp.path());
    const auto staged = repository.StageChartResult(attempt, {});
    if (staged.status != result_persistence::StageStatus::Staged &&
        staged.status != result_persistence::StageStatus::AlreadyStaged) {
      std::cerr << "FAIL: canonical replay staging: " << staged.diagnostic
                << '\n';
    }
    expect(staged.status == result_persistence::StageStatus::Staged ||
               staged.status ==
                   result_persistence::StageStatus::AlreadyStaged,
           "service fixture stores a canonical replay attempt");
    return repository.EnqueueReadyIrOutboxDraft(value, userIntent);
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
  std::atomic_int wallNowCalls{0};
  std::atomic<std::int64_t> steadyNow{5'000};
  std::atomic_bool useIndependentSteady{false};
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
  std::function<void()> wakeHook;
  std::function<void()> monotonicHook;
  mutable std::mutex projectionMutex;
  bool projectionShouldSucceed = true;
  int projectionCalls = 0;
  std::string projectedProfile;
  std::string projectedProvider;
  std::string projectedOrigin;
  std::int64_t projectedGeneration = 0;
  std::vector<std::string> projectedScoreIds;
  bool projectionSawCommittedMirror = false;
  std::unique_ptr<ir::IrSubmissionService> service;
};

ir::IrOutboxEntry load(Harness &harness, int suffix) {
  const auto result =
      harness.repository.LoadIrOutbox("fake", attemptId(suffix));
  expect(result.status == ir::IrOutboxReadStatus::Found && result.entry,
         "expected outbox row exists");
  return result.entry.value_or(ir::IrOutboxEntry{});
}

bool waitForReconciliationPhase(Harness &harness,
                                ir::IrReconciliationPhase phase) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (harness.service->reconciliationStatus("fake").phase == phase) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

std::optional<std::int64_t>
remoteMirrorGeneration(const std::filesystem::path &databasePath,
                       std::string_view providerId,
                       std::string_view serverOrigin) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(databasePath.string().c_str(), &database,
                      SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (database) {
      sqlite3_close(database);
    }
    return std::nullopt;
  }
  sqlite3_stmt *statement = nullptr;
  const char *query =
      "SELECT MIN(sync_generation),MAX(sync_generation) "
      "FROM ir_remote_scores WHERE provider_id=? AND server_origin=?";
  std::optional<std::int64_t> result;
  if (sqlite3_prepare_v2(database, query, -1, &statement, nullptr) ==
          SQLITE_OK &&
      sqlite3_bind_text(statement, 1, providerId.data(),
                        static_cast<int>(providerId.size()),
                        SQLITE_TRANSIENT) == SQLITE_OK &&
      sqlite3_bind_text(statement, 2, serverOrigin.data(),
                        static_cast<int>(serverOrigin.size()),
                        SQLITE_TRANSIENT) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_ROW &&
      sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
      sqlite3_column_type(statement, 1) == SQLITE_INTEGER &&
      sqlite3_column_int64(statement, 0) ==
          sqlite3_column_int64(statement, 1)) {
    result = sqlite3_column_int64(statement, 0);
  }
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return result;
}

bool executeSql(const std::filesystem::path &databasePath,
                std::string_view sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open(databasePath.string().c_str(), &database) != SQLITE_OK) {
    if (database) {
      sqlite3_close(database);
    }
    return false;
  }
  char *error = nullptr;
  const bool succeeded =
      sqlite3_exec(database, std::string(sql).c_str(), nullptr, nullptr,
                   &error) == SQLITE_OK;
  sqlite3_free(error);
  sqlite3_close(database);
  return succeeded;
}

bool irIdentityStorageContains(const std::filesystem::path &databasePath,
                               std::string_view value) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(databasePath.string().c_str(), &database,
                      SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (database) {
      sqlite3_close(database);
    }
    return true;
  }
  sqlite3_stmt *statement = nullptr;
  const char *query =
      "SELECT EXISTS(SELECT 1 FROM ir_outbox WHERE "
      "instr(ifnull(remote_job_id,'') || ifnull(remote_origin,'') || "
      "ifnull(last_error_code,'') || ifnull(last_error_message,''),?1)>0 "
      "UNION ALL SELECT 1 FROM ir_submission_receipts WHERE "
      "instr(ifnull(server_origin,'') || ifnull(remote_chart_id,'') || "
      "ifnull(remote_score_id,'') || ifnull(CAST(remote_user_id AS TEXT),''),"
      "?1)>0 LIMIT 1)";
  bool contains = true;
  if (sqlite3_prepare_v2(database, query, -1, &statement, nullptr) == SQLITE_OK &&
      sqlite3_bind_text(statement, 1, value.data(),
                        static_cast<int>(value.size()), SQLITE_TRANSIENT) ==
          SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_ROW) {
    contains = sqlite3_column_int(statement, 0) != 0;
  }
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return contains;
}

void makeFailed(Harness &harness, int suffix) {
  const auto inserted =
      harness.enqueueReady(draft(suffix, harness.now.load()), false);
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
  const auto inserted =
      harness.enqueueReady(draft(suffix, harness.now.load()), false);
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

void testDueAttemptsSubmitAsOneAtomicGroup() {
  Harness harness;
  harness.setCredential("key");
  for (int suffix = 200; suffix < 203; ++suffix) {
    expect(harness.enqueueReady(draft(suffix, harness.now.load()),
                                suffix == 201)
               .entry.has_value(),
           "grouped submit fixture inserts");
  }
  harness.driver->pushSubmit({
      .status = ir::DeliveryStatus::Succeeded,
      .remoteUserId = 42,
      .remoteScoreIds = {"score-1", "score-2", "score-3"},
  });
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));

  expect(harness.driver->waitForCalls(1), "one grouped request starts");
  const auto calls = harness.driver->calls();
  expect(calls.size() == 1 && calls.front().rowIds.size() == 3,
         "three due attempts share one provider request");
  expect(calls.front().userIntent,
         "grouped request consumes aggregate manual intent");
  for (int suffix = 200; suffix < 203; ++suffix) {
    expect(harness.waitForState(attemptId(suffix),
                                ir::IrOutboxState::Succeeded),
           "every grouped attempt publishes success");
    const auto receipt = harness.repository.LoadIrSubmissionReceipt(
        "fake", "https://boku.tachi.ac", attemptId(suffix));
    expect(receipt.status == ir::IrReceiptReadStatus::Found &&
               receipt.receipt && receipt.receipt->remoteScoreId.empty(),
           "multi-entry success stores no guessed remote score identity");
  }
  expect(harness.succeededCallbacks().size() == 3,
         "each committed grouped chart publishes its success callback");
}

void testMixedRequestKindsUseTwoPlannedCalls() {
  Harness harness;
  harness.setCredential("key");
  makeAwaiting(harness, 203);
  harness.now += 10'000;
  expect(harness.enqueueReady(draft(204, harness.now.load()), false)
             .entry.has_value(),
         "mixed-plan pending fixture inserts");
  harness.driver->pushPoll({.status = ir::DeliveryStatus::Succeeded});
  harness.driver->pushSubmit({.status = ir::DeliveryStatus::Succeeded});
  harness.service->start(profile(true));

  expect(harness.driver->waitForCalls(2),
         "mixed request kinds perform two planned calls");
  const auto calls = harness.driver->calls();
  expect(calls.size() == 2 && calls[0].poll != calls[1].poll &&
             calls[0].rowIds.size() == 1 && calls[1].rowIds.size() == 1,
         "poll and submit rows never share one provider request");
  expect(harness.waitForState(attemptId(203), ir::IrOutboxState::Succeeded) &&
             harness.waitForState(attemptId(204),
                                  ir::IrOutboxState::Succeeded),
         "both mixed planned groups complete");
}

void testSharedDeferredGroupPollsOnce() {
  Harness harness;
  harness.setCredential("key");
  for (int suffix = 205; suffix < 208; ++suffix) {
    expect(harness.enqueueReady(draft(suffix, harness.now.load()), false)
               .entry.has_value(),
           "shared deferred fixture inserts");
  }
  harness.driver->pushSubmit({
      .status = ir::DeliveryStatus::Deferred,
      .remoteJobId = "shared-job",
      .remoteOrigin = "https://boku.tachi.ac",
  });
  harness.driver->pushPoll({
      .status = ir::DeliveryStatus::Succeeded,
      .remoteUserId = 42,
      .remoteScoreIds = {"score-1", "score-2", "score-3"},
  });
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));

  expect(harness.driver->waitForCalls(1), "shared deferred submit starts");
  for (int suffix = 205; suffix < 208; ++suffix) {
    expect(harness.waitForState(attemptId(suffix),
                                ir::IrOutboxState::AwaitingRemoteResult),
           "shared deferred state is committed for every row");
  }
  harness.now += 200;
  harness.service->notifyOutboxChanged();
  expect(harness.driver->waitForCalls(2), "shared deferred poll starts");
  const auto calls = harness.driver->calls();
  expect(calls.size() == 2 && !calls[0].poll && calls[0].rowIds.size() == 3 &&
             calls[1].poll && calls[1].rowIds.size() == 3 &&
             calls[1].remoteJobId == "shared-job",
         "one shared remote job performs one grouped poll");
}

void testGroupedCancellationRecoversEveryClaim() {
  Harness harness;
  harness.setCredential("key");
  for (int suffix = 208; suffix < 211; ++suffix) {
    harness.enqueueReady(draft(suffix, harness.now.load()), false);
  }
  harness.driver->blockRequestsUntilCancelled();
  harness.service->start(profile(true));
  expect(harness.driver->waitForCalls(1), "grouped cancellation starts");
  expect(harness.driver->calls().front().rowIds.size() == 3,
         "grouped cancellation owns every planned row");

  harness.service->pauseAndCancel();
  for (int suffix = 208; suffix < 211; ++suffix) {
    const auto recovered = load(harness, suffix);
    expect(recovered.state == ir::IrOutboxState::Pending &&
               recovered.requestAttemptCount == 1,
           "grouped cancellation recovers every claimed row");
  }
  expect(harness.driver->calls().size() == 1,
         "grouped cancellation performs no replacement transport");
}

void testMissingCredentialBlocksWholePlannedGroup() {
  Harness harness;
  for (int suffix = 211; suffix < 214; ++suffix) {
    harness.enqueueReady(draft(suffix, harness.now.load()), suffix == 212);
  }
  harness.service->start(profile(true));
  for (int suffix = 211; suffix < 214; ++suffix) {
    expect(harness.waitForState(attemptId(suffix),
                                ir::IrOutboxState::BlockedConfiguration),
           "missing credential blocks every planned row");
    const auto blocked = load(harness, suffix);
    expect(blocked.requestAttemptCount == 0 &&
               blocked.nextRequestUserIntent == (suffix == 212),
           "credential block does not claim or consume grouped intent");
  }
  expect(harness.driver->calls().empty(),
         "missing grouped credential performs no transport");
}

void testAmbiguousPartialResultFailsWholeGroup() {
  Harness harness;
  harness.setCredential("key");
  for (int suffix = 214; suffix < 217; ++suffix) {
    harness.enqueueReady(draft(suffix, harness.now.load()), false);
  }
  harness.driver->pushSubmit({
      .status = ir::DeliveryStatus::PermanentFailure,
      .remoteUserId = 42,
      .remoteScoreIds = {"score-1"},
      .importHadErrors = true,
      .code = "ambiguous_partial_import",
      .diagnostic = "provider returned an ambiguous partial result",
  });
  harness.service->start(profile(true));

  expect(harness.driver->waitForCalls(1), "ambiguous grouped request starts");
  expect(harness.driver->calls().size() == 1 &&
             harness.driver->calls().front().rowIds.size() == 3,
         "ambiguous response belongs to one grouped transport");
  for (int suffix = 214; suffix < 217; ++suffix) {
    expect(harness.waitForState(attemptId(suffix),
                                ir::IrOutboxState::FailedPermanent),
           "ambiguous partial result fails every grouped row");
    expect(load(harness, suffix).lastErrorCode == "ambiguous_partial_import",
           "ambiguous partial failure is persisted consistently");
  }
}

void testIdentifiedInvalidAndUnsupportedPlansIsolateRejectedRow() {
  const auto run = [](ir::IrOutboxBatchPlanStatus planStatus, int firstSuffix,
                      int laterSuffix) {
    Harness harness;
    harness.setCredential("key");
    const auto first =
        harness.enqueueReady(draft(firstSuffix, harness.now.load()), false);
    const auto later =
        harness.enqueueReady(draft(laterSuffix, harness.now.load()), false);
    expect(first.entry && later.entry,
           "plan isolation fixtures insert in due order");
    harness.driver->rejectPlanForAttempt(attemptId(firstSuffix), planStatus);
    harness.driver->pushSubmit({.status = ir::DeliveryStatus::Succeeded});
    harness.service->start(profile(true, true, "https://boku.tachi.ac"));

    expect(harness.driver->waitForCalls(1),
           "a later valid row proceeds after a rejected first plan");
    expect(harness.waitForState(attemptId(firstSuffix),
                                ir::IrOutboxState::FailedPermanent) &&
               harness.waitForState(attemptId(laterSuffix),
                                    ir::IrOutboxState::Succeeded),
           "the rejected row is terminal while later due work succeeds");
    const auto calls = harness.driver->calls();
    expect(calls.size() == 1 && later.entry &&
               calls.front().rowIds ==
                   std::vector<std::int64_t>{later.entry->id},
           "plan rejection performs no transport and does not busy-loop");
  };

  run(ir::IrOutboxBatchPlanStatus::Invalid, 217, 218);
  run(ir::IrOutboxBatchPlanStatus::Unsupported, 219, 220);
}

void testIdentifiedLaterPlanRejectionNeverDiscardsTheFirstDueRow() {
  Harness harness;
  harness.setCredential("key");
  const auto first = harness.enqueueReady(draft(237, harness.now.load()), false);
  const auto rejected =
      harness.enqueueReady(draft(238, harness.now.load()), false);
  const auto later = harness.enqueueReady(draft(239, harness.now.load()), false);
  expect(first.entry && rejected.entry && later.entry,
         "identified later rejection fixtures insert in order");
  harness.driver->rejectPlanForAttempt(
      attemptId(237), ir::IrOutboxBatchPlanStatus::Invalid, true,
      rejected.entry ? std::optional(rejected.entry->id) : std::nullopt);
  harness.driver->pushSubmit({.status = ir::DeliveryStatus::Succeeded});
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));

  expect(harness.driver->waitForCalls(1),
         "valid work proceeds after an identified later-row rejection");
  expect(harness.waitForState(attemptId(237), ir::IrOutboxState::Succeeded) &&
             harness.waitForState(attemptId(238),
                                  ir::IrOutboxState::FailedPermanent) &&
             harness.waitForState(attemptId(239), ir::IrOutboxState::Succeeded),
         "only the planner-identified later row becomes terminal");
  const auto calls = harness.driver->calls();
  expect(calls.size() == 1 && first.entry && later.entry &&
             calls.front().rowIds == std::vector<std::int64_t>{
                                            first.entry->id, later.entry->id},
         "identified later rejection preserves all valid due rows");
}

void testUnidentifiedPlanFailureDoesNotDiscardAnyDueRow() {
  Harness harness;
  harness.setCredential("key");
  expect(harness.enqueueReady(draft(227, harness.now.load()), false).entry &&
             harness.enqueueReady(draft(228, harness.now.load()), false).entry,
         "unidentified plan failure fixtures insert");
  harness.driver->rejectPlanForAttempt(
      attemptId(227), ir::IrOutboxBatchPlanStatus::Invalid, false);
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));

  expect(harness.waiter.waitForEntries(1),
         "unidentified plan failure enters a bounded wait");
  const auto first = load(harness, 227);
  const auto second = load(harness, 228);
  expect(first.state == ir::IrOutboxState::Pending &&
             second.state == ir::IrOutboxState::Pending &&
             first.requestAttemptCount == 0 &&
             second.requestAttemptCount == 0,
         "unidentified plan failure leaves every due row untouched");
  expect(harness.driver->calls().empty(),
         "unidentified plan failure neither transports nor busy-loops");
}

void testTachiMalformedLaterRowDoesNotDiscardOrStarveValidWork() {
  TemporaryDirectory temp;
  ReplayRepository repository(temp.path() / "replays.db");
  expect(repository.EnsureSchema(), "Tachi service integration schema starts");
  ir::IrDriverRegistry registry;
  auto driver = std::make_shared<ir::tachi::TachiDriver>();
  std::string diagnostic;
  expect(registry.registerDriver(driver, diagnostic),
         "Tachi service integration driver registers");
  TachiSuccessHttpClient http;
  ManualWaiter waiter;
  const std::int64_t now = 1'000'000'000'000LL;
  ir::IrSubmissionServiceOptions options;
  options.wallNowUnixMillis = [=] { return now; };
  options.credentialLookup = [](std::string_view, std::string_view providerId) {
    return providerId == "tachi" ? std::string("key") : std::string{};
  };
  options.waitUntil = [&](std::stop_token token,
                          std::optional<std::chrono::steady_clock::time_point>
                              deadline) { waiter.wait(token, deadline); };
  options.wake = [&] { waiter.wake(); };
  ir::IrSubmissionService service(repository, registry, http,
                                  std::move(options));

  std::vector<ir::IrOutboxDraft> drafts{
      tachiDraft(229, now), tachiDraft(230, now), tachiDraft(231, now)};
  drafts[1].rulesetProof.validationFingerprint = std::string(64, 'e');
  for (const auto &value : drafts) {
    const auto staged = repository.StageChartResult(
        canonicalAttempt(value, temp.path()), {});
    expect(staged.status == result_persistence::StageStatus::Staged,
           "Tachi service integration stages a canonical attempt");
    expect(repository.EnqueueReadyIrOutboxDraft(value, false).entry.has_value(),
           "Tachi service integration enqueues its outbox row");
  }
  ir::IrActiveProfileConfig config{.profileId = "profile-a"};
  config.providers["tachi"] = {
      .enabled = true,
      .autoSubmit = true,
      .serverOrigin = "https://boku.tachi.ac",
  };
  service.start(std::move(config));

  expect(http.waitForRequests(2),
         "valid Tachi rows on both sides of corruption are delivered");
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  bool settled = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto first = repository.LoadIrOutbox("tachi", attemptId(229));
    const auto malformed = repository.LoadIrOutbox("tachi", attemptId(230));
    const auto later = repository.LoadIrOutbox("tachi", attemptId(231));
    settled = first.entry && malformed.entry && later.entry &&
              first.entry->state == ir::IrOutboxState::Succeeded &&
              malformed.entry->state == ir::IrOutboxState::FailedPermanent &&
              later.entry->state == ir::IrOutboxState::Succeeded;
    if (settled) {
      break;
    }
    std::this_thread::yield();
  }
  expect(settled,
         "only the explicitly malformed Tachi row becomes terminal");
  const auto requests = http.requests();
  expect(requests.size() == 2,
         "Tachi corruption causes no transport for the rejected row");
  service.stop();
  repository.Shutdown();
}

void testEchoedCredentialResponseIdentitiesAreNeverPersisted() {
  const std::string credential = "echoed-secret";
  const auto run = [&](int suffix, ir::DeliveryOutcome outcome,
                       std::string_view fieldName) {
    Harness harness;
    harness.setCredential(credential);
    expect(harness.enqueueReady(draft(suffix, harness.now.load()), false)
               .entry.has_value(),
           "credential echo fixture inserts");
    harness.driver->pushSubmit(std::move(outcome));
    harness.service->start(profile(true, true, "https://boku.tachi.ac"));

    expect(harness.waitForState(attemptId(suffix),
                                ir::IrOutboxState::FailedPermanent),
           std::string(fieldName) + " credential echo is rejected");
    const auto stored = load(harness, suffix);
    const auto receipt = harness.repository.LoadIrSubmissionReceipt(
        "fake", "https://boku.tachi.ac", attemptId(suffix));
    expect(stored.remoteJobId.empty() && stored.remoteOrigin.empty() &&
               stored.lastErrorCode == "malformed_response" &&
               stored.lastErrorMessage.find(credential) == std::string::npos,
           std::string(fieldName) +
               " credential echo is absent from the persisted outbox row");
    expect(receipt.status == ir::IrReceiptReadStatus::NotFound,
           std::string(fieldName) +
               " credential echo creates no submission receipt");
    expect(!irIdentityStorageContains(harness.temp.path() / "replays.db",
                                      credential),
           std::string(fieldName) +
               " credential echo is absent from identity-bearing DB columns");
  };

  run(221,
      {.status = ir::DeliveryStatus::Deferred,
       .remoteJobId = "job-" + credential,
       .remoteOrigin = "https://boku.tachi.ac"},
      "remoteJobId");
  run(222,
      {.status = ir::DeliveryStatus::Deferred,
       .remoteJobId = "safe-job",
       .remoteOrigin = "https://" + credential + ".example.test"},
      "remoteOrigin");
  run(223,
      {.status = ir::DeliveryStatus::Succeeded,
       .remoteUserId = 42,
       .remoteScoreId = "score-" + credential},
      "remoteScoreId");

  Harness numeric;
  numeric.setCredential("42");
  expect(numeric.enqueueReady(draft(232, numeric.now.load()), false)
             .entry.has_value(),
         "numeric credential echo fixture inserts");
  numeric.driver->pushSubmit({.status = ir::DeliveryStatus::Succeeded,
                              .remoteUserId = 42});
  numeric.service->start(profile(true, true, "https://boku.tachi.ac"));
  expect(numeric.waitForState(attemptId(232),
                              ir::IrOutboxState::FailedPermanent),
         "canonical numeric remoteUserId credential echo is rejected");
  const auto numericStored = load(numeric, 232);
  const auto numericReceipt = numeric.repository.LoadIrSubmissionReceipt(
      "fake", "https://boku.tachi.ac", attemptId(232));
  expect(numericStored.lastErrorCode == "malformed_response" &&
             numericReceipt.status == ir::IrReceiptReadStatus::NotFound &&
             !irIdentityStorageContains(numeric.temp.path() / "replays.db",
                                        "42"),
         "numeric credential echo is absent from outbox and receipt storage");
}

void testFailedAtomicDeliveryApplyRequeuesTheWholeClaimedGroup() {
  Harness harness;
  harness.setCredential("key");
  for (int suffix = 224; suffix < 227; ++suffix) {
    expect(harness.enqueueReady(draft(suffix, harness.now.load()), false)
               .entry.has_value(),
           "apply recovery fixture inserts");
  }
  expect(executeSql(
             harness.temp.path() / "replays.db",
             "CREATE TRIGGER fail_service_receipt_insert BEFORE INSERT ON "
             "ir_submission_receipts BEGIN SELECT RAISE(ABORT,'injected'); END"),
         "apply recovery fixture installs a receipt failure");
  harness.driver->pushSubmit({.status = ir::DeliveryStatus::Succeeded,
                              .remoteUserId = 42});
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));

  expect(harness.driver->waitForCalls(1),
         "apply recovery group performs one provider request");
  bool allRequeued = true;
  for (int suffix = 224; suffix < 227; ++suffix) {
    allRequeued =
        harness.waitForSnapshot(attemptId(suffix), [](const auto &snapshot) {
          return snapshot.state == ir::IrOutboxState::Pending &&
                 snapshot.consecutiveFailureCount == 1 &&
                 snapshot.nextAttemptAtUnixMillis.has_value();
        }) &&
        allRequeued;
  }
  expect(allRequeued,
         "failed atomic apply makes every claimed row processable again");
  for (int suffix = 224; suffix < 227; ++suffix) {
    const auto stored = load(harness, suffix);
    const auto receipt = harness.repository.LoadIrSubmissionReceipt(
        "fake", "https://boku.tachi.ac", attemptId(suffix));
    expect(stored.state == ir::IrOutboxState::Pending &&
               stored.lastErrorCode == "storage_apply_failed" &&
               receipt.status == ir::IrReceiptReadStatus::NotFound,
           "failed atomic apply leaves no partial receipt or terminal state");
  }
  expect(harness.driver->calls().size() == 1 &&
             harness.succeededCallbacks().empty(),
         "apply failure neither busy-loops nor publishes delivery success");
}

void testFailedExactGroupRecoveryNeverResetsUnrelatedUploadingRows() {
  Harness harness;
  harness.setCredential("key");
  std::vector<std::int64_t> groupRowIds;
  for (int suffix = 233; suffix < 236; ++suffix) {
    const auto inserted =
        harness.enqueueReady(draft(suffix, harness.now.load()), false);
    expect(inserted.entry.has_value(), "scoped recovery group fixture inserts");
    if (inserted.entry) {
      groupRowIds.push_back(inserted.entry->id);
    }
  }
  harness.driver->pushSubmit({.status = ir::DeliveryStatus::Succeeded,
                              .remoteUserId = 84});
  harness.driver->blockRequestsUntilReleased();
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));
  expect(harness.driver->waitForCalls(1),
         "scoped recovery group reaches the provider");
  const std::size_t waitsBeforeRecovery = harness.waiter.entryCount();

  const std::int64_t unrelatedId = makeAwaiting(harness, 236);
  harness.now += 10'000;
  expect(harness.repository
                 .ClaimIrOutbox(unrelatedId,
                                ir::IrOutboxState::AwaitingRemoteResult,
                                harness.now.load())
                 .status == ir::IrOutboxClaimStatus::Claimed,
         "unrelated deferred row is independently uploading");
  expect(groupRowIds.size() == 3,
         "scoped recovery trigger has every claimed group row");
  if (groupRowIds.size() == 3) {
    const std::string trigger =
        "CREATE TRIGGER fail_exact_group_recovery BEFORE UPDATE ON ir_outbox "
        "WHEN OLD.id IN (" +
        std::to_string(groupRowIds[0]) + "," +
        std::to_string(groupRowIds[1]) + "," +
        std::to_string(groupRowIds[2]) +
        ") AND OLD.state=1 AND (NEW.state=5 OR "
        "NEW.last_error_code='storage_apply_failed') BEGIN SELECT "
        "RAISE(ABORT,'injected exact recovery failure'); END";
    expect(executeSql(harness.temp.path() / "replays.db", trigger),
           "scoped recovery fixture installs an exact-group apply failure");
  }
  harness.driver->releaseBlockedRequests();
  expect(harness.waiter.waitForEntries(waitsBeforeRecovery + 1),
         "scoped recovery failure returns the worker to a bounded wait");

  const auto unrelatedAfter = load(harness, 236);
  expect(unrelatedAfter.state == ir::IrOutboxState::Uploading,
         "failed exact-group recovery never resets an unrelated claim");
  for (int suffix = 233; suffix < 236; ++suffix) {
    expect(load(harness, suffix).state == ir::IrOutboxState::Uploading,
           "failed exact-group recovery waits for lifecycle stale recovery");
  }
}

void testActiveRequestSnapshotsDistinguishSubmitAndPoll() {
  Harness submit;
  submit.setCredential("key");
  submit.enqueueReady(draft(20, submit.now.load()), false);
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
  const auto pending =
      harness.enqueueReady(draft(1, harness.now.load()), false);
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
  disabled.enqueueReady(draft(3, disabled.now.load()), false);
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
  readOnly.enqueueReady(draft(4, readOnly.now.load()), false);
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
        harness.enqueueReady(skipped, false).entry.has_value() &&
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
  const auto inserted = harness.enqueueReady(draft(5, harness.now.load()), true);
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
  harness.enqueueReady(draft(23, harness.now.load()), false);
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

void testManualBatchPublishesAndWakesOnceWithSingularCompatibility() {
  Harness harness;
  harness.service->setApplicationActive(false);
  harness.service->start(profile(true));

  std::atomic_int wakes{0};
  harness.wakeHook = [&] { ++wakes; };
  const int capabilityCalls = harness.driver->capabilitiesCalls();
  const int wallNowCalls = harness.wallNowCalls.load();
  const std::vector drafts{draft(40, harness.now.load()),
                           draft(41, harness.now.load() + 1)};

  const auto outcome = harness.service->enqueueManualBatch(drafts);

  expect(outcome.storageAvailable && outcome.items.size() == 2 &&
             outcome.items[0].status ==
                 ir::IrManualBatchItemStatus::Inserted &&
             outcome.items[1].status ==
                 ir::IrManualBatchItemStatus::Inserted,
         "manual batch persists every prepared draft in one mutation");
  expect(harness.driver->capabilitiesCalls() == capabilityCalls + 1,
         "manual batch checks provider availability once");
  expect(harness.wallNowCalls.load() == wallNowCalls + 1,
         "manual batch captures one safe mutation timestamp");
  expect(wakes.load() == 1, "manual batch wakes the worker once");
  const auto first = harness.service->status("fake", attemptId(40));
  const auto second = harness.service->status("fake", attemptId(41));
  expect(first.found && second.found && first.revision + 1 == second.revision,
         "manual batch publishes entries under one uninterrupted generation");
  const auto counts = harness.service->counts("fake");
  expect(counts.storageAvailable && counts.pending == 2 && counts.total == 2,
         "manual batch refreshes the provider count snapshot");

  wakes = 0;
  const auto singular =
      harness.service->enqueueManual(draft(42, harness.now.load() + 2));
  expect(singular.status == ir::IrOutboxInsertStatus::Inserted &&
             singular.entry && singular.entry->attemptId == attemptId(42),
         "singular manual enqueue delegates through the batch mutation");
  expect(wakes.load() == 1 &&
             harness.service->counts("fake").pending == 3,
         "singular compatibility retains one publish, refresh, and wake");
}

void testAutomaticAndManualRequestsUseCurrentOrigin() {
  Harness harness;
  harness.setCredential("current-key");
  harness.enqueueReady(draft(6, harness.now.load()), false);
  harness.enqueueReady(draft(7, harness.now.load() + 1), true);
  harness.enqueueReady(draft(18, harness.now.load() + 2), false);
  harness.service->start(profile(true, true, "https://new.example.test"));
  expect(harness.driver->waitForCalls(1),
         "the worker groups every due automatic and manual row");
  const auto calls = harness.driver->calls();
  expect(calls.size() == 1 && calls[0].rowIds.size() == 3 &&
             calls[0].userIntent,
         "grouped POST carries aggregate manual intent");
  expect(calls.size() == 1 &&
             calls[0].configuredOrigin == "https://new.example.test",
         "grouped pending rows use the current configured origin");
}

void testDeferredPollingPinsOriginAndNeverReposts() {
  Harness harness;
  harness.setCredential("key");
  const auto inserted =
      harness.enqueueReady(draft(8, harness.now.load()), false);
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
             awaiting.remotePollCount == 0 &&
             awaiting.nextAttemptAtUnixMillis == harness.now.load() + 200,
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
  expect(awaiting.remotePollCount == 1 &&
             awaiting.nextAttemptAtUnixMillis == harness.now.load() + 1'000,
         "ongoing response advances the adaptive polling stage");
  expect(harness.service->retry(awaiting.id).status ==
             ir::IrOutboxMutationStatus::Updated,
         "second deferred retry is accepted");
  expect(harness.driver->waitForCalls(3), "second deferred retry polls");
  calls = harness.driver->calls();
  expect(!calls[0].poll && calls[1].poll && calls[2].poll,
         "awaiting row is POSTed once and polled thereafter");
  expect(harness.waitForState(attemptId(8), ir::IrOutboxState::Succeeded),
         "completed poll succeeds");
  const auto callbackDeadline = std::chrono::steady_clock::now() + 3s;
  while (harness.succeededCallbacks().empty() &&
         std::chrono::steady_clock::now() < callbackDeadline) {
    std::this_thread::yield();
  }
  const auto callbacks = harness.succeededCallbacks();
  expect(callbacks.size() == 1 &&
             callbacks.front().find("https://old.example.test") !=
                 std::string::npos,
         "completion invalidates ranking cache at persisted origin");
}

void testAdaptiveDeferredPollingCadence() {
  Harness harness;
  harness.setCredential("key");
  const auto inserted =
      harness.enqueueReady(draft(24, harness.now.load()), false);
  expect(inserted.entry.has_value(), "adaptive polling fixture inserts");
  harness.driver->pushSubmit({
      .status = ir::DeliveryStatus::Deferred,
      .remoteJobId = "job-adaptive",
      .remoteOrigin = "https://old.example.test",
  });
  for (int index = 0; index < 6; ++index) {
    harness.driver->pushPoll({.status = ir::DeliveryStatus::Ongoing});
  }

  harness.service->start(profile(true));
  expect(harness.waitForSnapshot(
             attemptId(24), [](const auto &snapshot) {
               return snapshot.state ==
                          ir::IrOutboxState::AwaitingRemoteResult &&
                      snapshot.requestAttemptCount == 1;
             }),
         "accepted upload reaches its first deferred wait");
  auto awaiting = load(harness, 24);
  expect(awaiting.remotePollCount == 0 &&
             awaiting.nextAttemptAtUnixMillis == harness.now.load() + 200,
         "first remote poll is scheduled after 200 ms");

  const std::vector<std::int64_t> delays{1'000, 2'000, 3'000,
                                         5'000, 10'000, 10'000};
  for (std::size_t index = 0; index < delays.size(); ++index) {
    harness.now = *awaiting.nextAttemptAtUnixMillis;
    harness.service->notifyOutboxChanged();
    expect(harness.driver->waitForCalls(index + 2),
           "scheduled adaptive poll reaches the driver");
    expect(harness.waitForSnapshot(
               attemptId(24), [&](const auto &snapshot) {
                 return snapshot.state ==
                            ir::IrOutboxState::AwaitingRemoteResult &&
                        snapshot.requestAttemptCount >=
                            static_cast<int>(index + 2);
               }),
           "ongoing adaptive poll is persisted");
    awaiting = load(harness, 24);
    expect(awaiting.remotePollCount == static_cast<int>(index + 1) &&
               awaiting.nextAttemptAtUnixMillis ==
                   harness.now.load() + delays[index],
           "ongoing polls advance through the capped adaptive cadence");
  }
}

void testDeferredInitialPollIgnoresEarlierPostFailures() {
  Harness harness;
  harness.setCredential("key");
  harness.enqueueReady(draft(25, harness.now.load()), false);
  harness.driver->pushSubmit({.status = ir::DeliveryStatus::TransientFailure});
  harness.driver->pushSubmit({.status = ir::DeliveryStatus::TransientFailure});
  harness.driver->pushSubmit({
      .status = ir::DeliveryStatus::Deferred,
      .remoteJobId = "job-after-retries",
      .remoteOrigin = "https://old.example.test",
  });

  harness.service->start(profile(true));
  const std::vector<std::int64_t> postDelays{10'000, 30'000};
  for (std::size_t index = 0; index < postDelays.size(); ++index) {
    expect(harness.driver->waitForCalls(index + 1),
           "transient POST reaches the driver");
    expect(harness.waitForSnapshot(
               attemptId(25), [&](const auto &snapshot) {
                 return snapshot.state == ir::IrOutboxState::Pending &&
                        snapshot.consecutiveFailureCount ==
                            static_cast<int>(index + 1);
               }),
           "transient POST backoff is persisted");
    harness.now += postDelays[index];
    harness.service->notifyOutboxChanged();
  }
  expect(harness.driver->waitForCalls(3),
         "POST succeeds after earlier transport failures");
  expect(harness.waitForSnapshot(
             attemptId(25), [](const auto &snapshot) {
               return snapshot.state ==
                          ir::IrOutboxState::AwaitingRemoteResult &&
                      snapshot.requestAttemptCount == 3;
             }),
         "deferred response is persisted after POST retries");
  const auto awaiting = load(harness, 25);
  expect(awaiting.remotePollCount == 0 &&
             awaiting.nextAttemptAtUnixMillis == harness.now.load() + 200,
         "earlier POST failures do not skip the 200 ms first poll");
}

void testPersistedBackoffAndRetryAfter() {
  Harness harness;
  harness.setCredential("key");
  harness.enqueueReady(draft(9, harness.now.load()), false);
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
  retryAfter.enqueueReady(draft(10, retryAfter.now.load()), false);
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
  const auto pending =
      harness.enqueueReady(draft(22, harness.now.load() + 60'000), false);
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
  permanent.enqueueReady(draft(14, permanent.now.load()), false);
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

void testSuccessfulDeliveryPersistsRemoteReceiptBeforeStatus() {
  Harness submitted;
  submitted.setCredential("key");
  submitted.enqueueReady(draft(26, submitted.now.load()), false);
  submitted.driver->pushSubmit({
      .status = ir::DeliveryStatus::Succeeded,
      .remoteUserId = 42,
      .remoteScoreId = "Tscore",
  });
  submitted.service->start(
      profile(true, true, "HTTPS://BOKU.TACHI.AC:443/"));
  expect(submitted.waitForState(attemptId(26),
                                ir::IrOutboxState::Succeeded),
         "successful delivery publishes succeeded status");
  const auto submittedReceipt = submitted.repository.LoadIrSubmissionReceipt(
      "fake", "https://boku.tachi.ac", attemptId(26));
  const auto submittedStatus = submitted.service->status("fake", attemptId(26));
  expect(submittedReceipt.status == ir::IrReceiptReadStatus::Found &&
             submittedReceipt.receipt &&
             submittedReceipt.receipt->remoteUserId == 42 &&
             submittedReceipt.receipt->remoteScoreId == "Tscore" &&
             submittedReceipt.receipt->source ==
                 ir::IrReceiptConfirmationSource::Submission &&
             !submittedReceipt.receipt->observedInSnapshot &&
             submittedStatus.found &&
             submittedStatus.state == ir::IrOutboxState::Succeeded,
         "published success has a durable normalized-origin receipt");

  Harness duplicate;
  duplicate.setCredential("key");
  duplicate.enqueueReady(draft(27, duplicate.now.load()), false);
  duplicate.driver->pushSubmit({
      .status = ir::DeliveryStatus::Succeeded,
      .remoteUserId = 84,
      .code = "duplicate_score",
  });
  duplicate.service->start(profile(true, true, "https://boku.tachi.ac"));
  expect(duplicate.waitForState(attemptId(27),
                                ir::IrOutboxState::Succeeded),
         "idempotent duplicate publishes succeeded status");
  const auto duplicateReceipt = duplicate.repository.LoadIrSubmissionReceipt(
      "fake", "https://boku.tachi.ac", attemptId(27));
  expect(duplicateReceipt.status == ir::IrReceiptReadStatus::Found &&
             duplicateReceipt.receipt &&
             duplicateReceipt.receipt->remoteUserId == 84 &&
             duplicateReceipt.receipt->remoteScoreId.empty(),
         "idempotent duplicate without score ID still creates a receipt");
}

void testInvalidSuccessfulReceiptIdentityCompletesAsFailure() {
  Harness harness;
  harness.setCredential("key");
  harness.enqueueReady(draft(28, harness.now.load()), false);
  harness.driver->pushSubmit({
      .status = ir::DeliveryStatus::Succeeded,
      .remoteUserId = 42,
      .remoteScoreId = std::string("score\x01id", 8),
  });
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));

  expect(harness.waitForState(attemptId(28),
                              ir::IrOutboxState::FailedPermanent),
         "invalid successful identity completes as a permanent failure");
  const auto stored = load(harness, 28);
  expect(stored.state == ir::IrOutboxState::FailedPermanent &&
             stored.lastErrorCode == "malformed_response",
         "invalid successful identity does not leave a claimed row stuck");
  expect(harness.repository
             .LoadIrSubmissionReceipt("fake", "https://boku.tachi.ac",
                                      attemptId(28))
             .status == ir::IrReceiptReadStatus::NotFound,
         "invalid successful identity creates no receipt");
}

void testCredentialMutationWaitsForOldAccountWorkBeforeClearingEvidence() {
  const auto run = [](bool removeCredential) {
    Harness harness;
    harness.setCredential("old-key");
    harness.enqueueReady(draft(removeCredential ? 30 : 29, harness.now.load()),
                         false);
    const std::string attempt = attemptId(removeCredential ? 30 : 29);
    harness.driver->pushSubmit({
        .status = ir::DeliveryStatus::Succeeded,
        .remoteUserId = 42,
        .remoteScoreId = "old-account-score",
    });
    harness.driver->blockRequestsUntilReleased();
    harness.service->start(profile(true, true, "https://boku.tachi.ac"));
    expect(harness.driver->waitForCalls(1),
           "old-account request reaches the blocked driver");

    std::mutex orderMutex;
    std::condition_variable orderChanged;
    bool quiesceEntered = false;
    bool invalidationEntered = false;
    std::vector<std::string> order;
    ir::IrSettingsActionDependencies dependencies{
        .quiesceRemoteWork =
            [&](std::string &) {
              {
                std::lock_guard lock(orderMutex);
                quiesceEntered = true;
                order.emplace_back("quiesce");
                orderChanged.notify_all();
              }
              harness.service->pauseAndCancel();
              return true;
            },
        .invalidateProviderIdentity =
            [&](std::string_view providerId, std::string &diagnostic) {
              {
                std::lock_guard lock(orderMutex);
                invalidationEntered = true;
                order.emplace_back("invalidate");
                orderChanged.notify_all();
              }
              const auto cleared =
                  harness.repository.ClearIrProviderAccountEvidence(providerId);
              diagnostic = cleared.diagnostic;
              return cleared.status == ir::IrOutboxMutationStatus::Updated ||
                     cleared.status == ir::IrOutboxMutationStatus::NotFound;
            },
        .replaceCredential =
            [&](std::string_view key, std::string &) {
              {
                std::lock_guard lock(orderMutex);
                order.emplace_back("replace");
              }
              harness.setCredential(std::string(key));
              return true;
            },
        .removeCredential =
            [&](std::string &) {
              {
                std::lock_guard lock(orderMutex);
                order.emplace_back("remove");
              }
              harness.setCredential({});
              return true;
            },
        .credentialCommitted =
            [&] {
              std::lock_guard lock(orderMutex);
              order.emplace_back("committed");
            },
        .reactivateRemoteWork =
            [&](std::string &) {
              {
                std::lock_guard lock(orderMutex);
                order.emplace_back("reactivate");
              }
              harness.service->activateProfile(
                  profile(true, true, "https://boku.tachi.ac"));
              return true;
            },
    };
    ir::IrSettingsActionModel model(
        "fake", {.scoreSubmission = true, .deferredSubmission = true},
        {.enabled = true,
         .autoSubmit = true,
         .serverOrigin = "https://boku.tachi.ac"},
        true, std::move(dependencies));

    std::optional<ir::IrSettingsActionResult> actionResult;
    std::thread mutation([&] {
      actionResult = removeCredential ? model.removeCredential()
                                      : model.replaceCredential("new-key");
    });
    {
      std::unique_lock lock(orderMutex);
      expect(orderChanged.wait_for(lock, 3s, [&] { return quiesceEntered; }),
             "credential mutation enters synchronous quiescence");
      expect(!invalidationEntered,
             "receipt invalidation cannot run while old-account I/O is blocked");
    }

    harness.driver->releaseBlockedRequests();
    mutation.join();

    expect(actionResult && actionResult->succeeded(),
           "credential mutation succeeds after old-account work quiesces");
    expect(harness.repository
               .LoadIrSubmissionReceipt("fake", "https://boku.tachi.ac",
                                        attempt)
               .status == ir::IrReceiptReadStatus::NotFound,
           "old-account completion cannot recreate a cleared receipt");
    expect(harness.repository.LoadIrOutbox("fake", attempt).status ==
               ir::IrOutboxReadStatus::NotFound,
           "receipt-backed old-account success evidence is removed");
    const auto calls = harness.driver->calls();
    expect(calls.size() == 1 && calls.front().apiKey == "old-key",
           "reactivation does not replay cleared success with the new account");
    const std::vector<std::string> expectedOrder =
        removeCredential
            ? std::vector<std::string>{"quiesce", "invalidate", "remove",
                                       "committed", "reactivate"}
            : std::vector<std::string>{"quiesce", "invalidate", "replace",
                                       "committed", "reactivate"};
    expect(order == expectedOrder,
           "credential mutation stays quiesced through evidence and key changes");
  };

  run(false);
  run(true);
}

void testSucceededPurgeAndSnapshotReads() {
  Harness harness;
  const auto old = harness.enqueueReady(
      draft(15, harness.now.load() - 9LL * 24 * 60 * 60 * 1000), false);
  const auto recent = harness.enqueueReady(
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
        .successfulReceipt =
            ir::IrSuccessfulReceiptDraft{
                .serverOrigin = "https://old.example.test",
                .confirmedAtUnixMillis = inserted->entry->createdAtUnixMillis,
            },
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
  harness.enqueueReady(draft(17, harness.now.load()), false);
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
  const auto inserted =
      harness.enqueueReady(draft(18, harness.now.load()), false);
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
  harness.enqueueReady(draft(19, harness.now.load()), false);
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
  harness.enqueueReady(draft(22, harness.now.load()), false);
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

void testReconciliationPublishesEverySuccessfulWorkerPhase() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  harness.service->start(
      profile(true, true, "HTTPS://BOKU.TACHI.AC:443/"));
  expect(harness.waiter.waitForEntries(1),
         "reconciliation fixture worker reaches idle wait");

  std::mutex phasesMutex;
  std::vector<ir::IrReconciliationStatusSnapshot> observed;
  const auto observe = [&] {
    const auto snapshot = harness.service->reconciliationStatus("fake");
    std::lock_guard lock(phasesMutex);
    if (observed.empty() || observed.back().revision != snapshot.revision) {
      observed.push_back(snapshot);
    }
  };
  harness.wakeHook = observe;
  harness.monotonicHook = observe;

  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted,
         "configured reconciliation request is accepted");
  expect(harness.driver->waitForReconciliationStage(1),
         "reconciliation reaches the 7K request");
  observe();
  harness.driver->releaseReconciliationStage(1);
  expect(harness.driver->waitForReconciliationStage(2),
         "reconciliation reaches the 14K request");
  observe();
  harness.driver->releaseReconciliationStage(2);

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    observe();
    if (harness.service->reconciliationStatus("fake").phase ==
        ir::IrReconciliationPhase::Succeeded) {
      break;
    }
    std::this_thread::yield();
  }
  observe();

  std::vector<ir::IrReconciliationPhase> phases;
  std::uint64_t previousRevision = 0;
  {
    std::lock_guard lock(phasesMutex);
    for (const auto &snapshot : observed) {
      expect(snapshot.revision > previousRevision,
             "every observed reconciliation change increments revision");
      previousRevision = snapshot.revision;
      if (phases.empty() || phases.back() != snapshot.phase) {
        phases.push_back(snapshot.phase);
      }
    }
  }
  const std::vector expected{
      ir::IrReconciliationPhase::Queued,
      ir::IrReconciliationPhase::Fetching7K,
      ir::IrReconciliationPhase::Fetching14K,
      ir::IrReconciliationPhase::Applying,
      ir::IrReconciliationPhase::Succeeded,
  };
  expect(phases == expected,
         "one worker command publishes queued, both fetches, apply, success");
  const auto completed = harness.service->reconciliationStatus("fake");
  expect(harness.driver->reconciliationCalls() == 1 &&
             completed.remoteScores == 0 && completed.nextAllowedAt.has_value(),
         "successful empty snapshot completes once and starts cooldown");
}

void testQueuedReconciliationRejectsAChangedCredentialGeneration() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("old-record-sync-key");
  harness.enqueueReady(draft(31, harness.now.load()), false);
  harness.driver->blockRequestsUntilCancelled();
  harness.service->start(
      profile(true, true, "https://boku.tachi.ac"));
  expect(harness.driver->waitForCalls(1),
         "credential-generation fixture starts older outbox work");
  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted,
         "reconciliation queues behind the active outbox request");

  harness.setCredential("replacement-record-sync-key");
  harness.service->notifyConfigurationChanged();

  bool failed = false;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    failed = harness.service->reconciliationStatus("fake").phase ==
             ir::IrReconciliationPhase::Failed;
    if (failed || harness.driver->reconciliationCalls() != 0) {
      break;
    }
    std::this_thread::yield();
  }
  if (harness.driver->reconciliationCalls() != 0) {
    harness.driver->releaseReconciliationStage(2);
  }

  expect(failed && harness.driver->reconciliationCalls() == 0,
         "queued reconciliation cannot rebind to a replacement credential");
  const auto mirror = harness.repository.ListIrRemoteScores(
      "fake", "https://boku.tachi.ac");
  expect(mirror.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
             mirror.scores.empty(),
         "credential-generation cancellation leaves the remote mirror alone");
}

void testPauseCancelsAQueuedReconciliationBeforeAnyApply() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  harness.enqueueReady(draft(32, harness.now.load()), false);
  harness.driver->blockRequestsUntilCancelled();
  harness.service->start(
      profile(true, true, "https://boku.tachi.ac"));
  expect(harness.driver->waitForCalls(1),
         "pause fixture starts older outbox work");
  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted,
         "pause fixture queues reconciliation behind outbox work");
  const auto queued = harness.service->reconciliationStatus("fake");

  harness.service->pauseAndCancel();

  const auto cancelled = harness.service->reconciliationStatus("fake");
  expect(cancelled.phase == ir::IrReconciliationPhase::Failed &&
             cancelled.revision > queued.revision &&
             !cancelled.diagnostic.empty(),
         "pausing publishes bounded cancellation for queued reconciliation");
  const auto mirror = harness.repository.ListIrRemoteScores(
      "fake", "https://boku.tachi.ac");
  expect(harness.driver->reconciliationCalls() == 0 &&
             mirror.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
             mirror.scores.empty(),
         "paused queued reconciliation performs no fetch or repository apply");
}

void testReconciliationCoalescesAndSerializesNewOutboxDelivery() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  harness.service->start(
      profile(true, true, "https://boku.tachi.ac"));
  expect(harness.waiter.waitForEntries(1),
         "serialization fixture worker reaches idle wait");
  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted &&
             harness.driver->waitForReconciliationStage(1),
         "serialization fixture starts reconciliation");
  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::AlreadyRunning,
         "a running reconciliation coalesces another tap");

  const auto inserted =
      harness.enqueueReady(draft(33, harness.now.load()), false);
  expect(inserted.entry.has_value(),
         "new outbox work is durable during reconciliation");
  harness.service->notifyOutboxChanged();
  std::this_thread::yield();
  expect(harness.driver->calls().empty() &&
             harness.driver->reconciliationCalls() == 1,
         "outbox HTTP cannot overlap the reconciliation operation");

  harness.driver->releaseReconciliationStage(2);
  expect(waitForReconciliationPhase(harness,
                                    ir::IrReconciliationPhase::Succeeded),
         "serialized reconciliation completes");
  expect(harness.driver->waitForCalls(1) &&
             harness.waitForState(attemptId(33),
                                  ir::IrOutboxState::Succeeded),
         "new outbox work runs immediately after reconciliation without starvation");
  expect(harness.driver->reconciliationCalls() == 1,
         "coalesced tap never creates a second reconciliation call");
}

void testReconciliationUsesExactMonotonicCooldownAfterSuccessAndFailure() {
  const auto run = [](bool failFirst) {
    Harness harness({.readOnly = false,
                     .chartRankings = false,
                     .scoreSubmission = true,
                     .deferredSubmission = true,
                     .scoreReconciliation = true});
    harness.useIndependentSteady = true;
    harness.steadyNow = failFirst ? 7'000 : 5'000;
    harness.setCredential("record-sync-secret");
    if (failFirst) {
      harness.driver->pushReconciliation({
          .status = ir::IrUserScoreSnapshotStatus::TransientFailure,
          .code = "transport_error",
          .diagnostic = "provider echoed record-sync-secret",
      });
    }
    harness.driver->releaseReconciliationStage(2);
    harness.service->start(
        profile(true, true, "https://boku.tachi.ac"));
    expect(harness.service->requestUserScoreReconciliation("fake") ==
               ir::IrReconciliationRequestStatus::Accepted,
           "cooldown fixture accepts its first run");
    const auto terminalPhase = failFirst ? ir::IrReconciliationPhase::Failed
                                         : ir::IrReconciliationPhase::Succeeded;
    expect(waitForReconciliationPhase(harness, terminalPhase),
           "cooldown fixture reaches its terminal phase");
    const auto completed = harness.service->reconciliationStatus("fake");
    const auto expectedExpiry = std::chrono::steady_clock::time_point(
        std::chrono::milliseconds(harness.steadyNow.load() + 60'000));
    expect(completed.nextAllowedAt == expectedExpiry,
           "cooldown starts exactly sixty seconds after completion");
    if (failFirst) {
      expect(completed.diagnostic == "provider echoed [redacted]" &&
                 completed.diagnostic.find("record-sync-secret") ==
                     std::string::npos,
             "failed reconciliation publishes a bounded secret-free diagnostic");
    }

    harness.now += 24LL * 60 * 60 * 1000;
    expect(harness.service->requestUserScoreReconciliation("fake") ==
               ir::IrReconciliationRequestStatus::Cooldown,
           "wall-clock changes cannot expire reconciliation cooldown");
    harness.steadyNow += 59'999;
    expect(harness.service->requestUserScoreReconciliation("fake") ==
               ir::IrReconciliationRequestStatus::Cooldown,
           "cooldown still rejects one millisecond before expiry");
    ++harness.steadyNow;
    expect(harness.service->requestUserScoreReconciliation("fake") ==
               ir::IrReconciliationRequestStatus::Accepted,
           "cooldown accepts a new run exactly at monotonic expiry");
    expect(harness.driver->waitForReconciliationCalls(2),
           "accepted expiry request reaches the driver once");
  };

  run(false);
  run(true);
}

void testProfileAndOriginChangeDropAnInflightSnapshotBeforeApply() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  const auto seeded = harness.repository.ApplyIrRemoteSnapshot({
      .providerId = "fake",
      .serverOrigin = "https://boku.tachi.ac",
      .synchronizedAtUnixMillis = harness.now.load(),
      .scores = {remoteScore("existing-remote-score")},
  });
  expect(seeded.status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied,
         "profile-change fixture seeds the prior remote mirror");
  harness.driver->ignoreReconciliationCancellation();
  harness.service->start(
      profile(true, true, "https://boku.tachi.ac"));
  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted &&
             harness.driver->waitForReconciliationStage(1),
         "profile-change fixture starts reconciliation");

  auto replacement = profile(true, true, "https://new.example.test");
  replacement.profileId = "profile-b";
  std::thread switchProfile([&] {
    harness.service->activateProfile(std::move(replacement));
  });
  expect(harness.driver->waitForReconciliationCancellation(),
         "profile change requests cancellation of the old snapshot");
  harness.driver->releaseReconciliationStage(2);
  switchProfile.join();

  const auto oldMirror = harness.repository.ListIrRemoteScores(
      "fake", "https://boku.tachi.ac");
  const auto newMirror = harness.repository.ListIrRemoteScores(
      "fake", "https://new.example.test");
  const auto status = harness.service->reconciliationStatus("fake");
  expect(oldMirror.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
             newMirror.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
             oldMirror.scores.size() == 1 &&
             oldMirror.scores.front().remoteScoreId ==
                 "existing-remote-score" &&
             newMirror.scores.empty(),
         "late old-profile snapshot cannot replace either origin mirror");
  expect(status.phase == ir::IrReconciliationPhase::Idle &&
             harness.driver->reconciliationCalls() == 1,
         "replacement profile starts with idle reconciliation state");
}

void testReconciliationLoadsPlansAndAppliesOneCompleteSnapshot() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  const auto local = harness.enqueueReady(draft(34, harness.now.load()), false);
  expect(local.entry.has_value(),
         "planner fixture stores an eligible local replay");
  if (local.entry) {
    expect(harness.repository.DiscardIrOutbox(local.entry->id).status ==
               ir::IrOutboxMutationStatus::Updated,
           "planner fixture leaves the replay without requested outbox work");
  }
  harness.driver->pushReconciliation({
      .status = ir::IrUserScoreSnapshotStatus::Succeeded,
      .snapshot = ir::IrUserScoreSnapshot{.scores = {remoteScore()}},
  });
  harness.driver->releaseReconciliationStage(2);
  harness.service->start(
      profile(true, true, "https://boku.tachi.ac"));

  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted &&
             waitForReconciliationPhase(harness,
                                        ir::IrReconciliationPhase::Succeeded),
         "complete snapshot reconciles successfully");
  const auto completed = harness.service->reconciliationStatus("fake");
  const auto receipt = harness.repository.LoadIrSubmissionReceipt(
      "fake", "https://boku.tachi.ac", attemptId(34));
  const auto mirror = harness.repository.ListIrRemoteScores(
      "fake", "https://boku.tachi.ac");
  const auto generation = remoteMirrorGeneration(
      harness.temp.path() / "replays.db", "fake", "https://boku.tachi.ac");
  expect(receipt.status == ir::IrReceiptReadStatus::Found && receipt.receipt &&
             receipt.receipt->remoteScoreId == "remote-score-1" &&
             receipt.receipt->observedInSnapshot,
         "candidate load and pure planner repair the matching durable receipt");
  expect(mirror.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
             mirror.scores.size() == 1 && completed.remoteScores == 1 &&
             completed.remoteScoresAdded == 1 &&
             completed.receiptsUpserted == 1,
         "one atomic apply publishes its remote and receipt mutation counts");
  expect(generation == harness.now.load(),
         "fresh reconciliation invokes atomic snapshot apply exactly once");
  {
    std::lock_guard lock(harness.projectionMutex);
    expect(harness.projectionCalls == 1 &&
               harness.projectedProfile == "profile-a" &&
               harness.projectedProvider == "fake" &&
               harness.projectedOrigin == "https://boku.tachi.ac" &&
               harness.projectedGeneration == generation &&
               harness.projectedScoreIds ==
                   std::vector<std::string>{"remote-score-1"} &&
               harness.projectionSawCommittedMirror,
           "successful reconciliation projects the committed mirror once");
  }
}

void testProjectionFailurePublishesFailedButKeepsCommittedMirror() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  harness.projectionShouldSucceed = false;
  harness.driver->pushReconciliation({
      .status = ir::IrUserScoreSnapshotStatus::Succeeded,
      .snapshot = ir::IrUserScoreSnapshot{
          .scores = {remoteScore("projection-failure-score")}},
  });
  harness.driver->releaseReconciliationStage(2);
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));

  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted &&
             waitForReconciliationPhase(harness,
                                        ir::IrReconciliationPhase::Failed),
         "score projection failure publishes failed reconciliation");
  const auto failed = harness.service->reconciliationStatus("fake");
  const auto mirror = harness.repository.ListIrRemoteScores(
      "fake", "https://boku.tachi.ac");
  expect(failed.diagnostic == "could not project synchronized scores" &&
             mirror.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
             mirror.scores.size() == 1 &&
             mirror.scores.front().remoteScoreId ==
                 "projection-failure-score",
         "projection failure leaves the durable mirror available for retry");
  std::lock_guard lock(harness.projectionMutex);
  expect(harness.projectionCalls == 1 &&
             harness.projectionSawCommittedMirror,
         "failing projection observes the committed mirror exactly once");
}

void testEmptyReconciliationStillProjectsDeletionSnapshot() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  harness.driver->pushReconciliation({
      .status = ir::IrUserScoreSnapshotStatus::Succeeded,
      .snapshot = ir::IrUserScoreSnapshot{},
  });
  harness.driver->releaseReconciliationStage(2);
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));
  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted &&
             waitForReconciliationPhase(harness,
                                        ir::IrReconciliationPhase::Succeeded),
         "empty remote snapshot reconciles successfully");
  std::lock_guard lock(harness.projectionMutex);
  expect(harness.projectionCalls == 1 &&
             harness.projectedScoreIds.empty() &&
             harness.projectedGeneration > 0 &&
             harness.projectionSawCommittedMirror,
         "empty snapshot reaches projection to delete stale imported rows");
}

void testReconciliationPreservesSucceededWorkDeliveredToAnotherOrigin() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  const auto delivered =
      harness.enqueueReady(draft(36, harness.now.load()), false);
  expect(delivered.entry.has_value(),
         "cross-origin fixture stores a canonical outbox row");
  if (!delivered.entry) {
    return;
  }
  expect(harness.repository
                 .ClaimIrOutbox(delivered.entry->id,
                                ir::IrOutboxState::Pending,
                                harness.now.load())
                 .status == ir::IrOutboxClaimStatus::Claimed,
         "cross-origin fixture claims the outbox row");
  expect(harness.repository
                 .ApplyIrOutboxDelivery({
                     .rowId = delivered.entry->id,
                     .nextState = ir::IrOutboxState::Succeeded,
                     .updatedAtUnixMillis = harness.now.load(),
                     .completedAtUnixMillis = harness.now.load(),
                     .successfulReceipt =
                         ir::IrSuccessfulReceiptDraft{
                             .serverOrigin = "https://other.example",
                             .remoteUserId = 42,
                             .remoteScoreId = "other-origin-score",
                             .confirmedAtUnixMillis = harness.now.load(),
                         },
                 })
                 .status == ir::IrOutboxMutationStatus::Updated,
         "cross-origin fixture retains successful delivery evidence");

  harness.driver->pushReconciliation({
      .status = ir::IrUserScoreSnapshotStatus::Succeeded,
      .snapshot = ir::IrUserScoreSnapshot{.scores = {remoteScore()}},
  });
  harness.driver->releaseReconciliationStage(2);
  harness.service->start(profile(true, true, "https://boku.tachi.ac"));
  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted &&
             waitForReconciliationPhase(harness,
                                        ir::IrReconciliationPhase::Succeeded),
         "origin-A reconciliation completes against matching remote proof");

  const auto originAReceipt = harness.repository.LoadIrSubmissionReceipt(
      "fake", "https://boku.tachi.ac", attemptId(36));
  const auto originBReceipt = harness.repository.LoadIrSubmissionReceipt(
      "fake", "https://other.example", attemptId(36));
  const auto retainedOutbox =
      harness.repository.LoadIrOutbox("fake", attemptId(36));
  const auto completed = harness.service->reconciliationStatus("fake");
  expect(originAReceipt.status == ir::IrReceiptReadStatus::Found &&
             originAReceipt.receipt &&
             originAReceipt.receipt->remoteScoreId == "remote-score-1",
         "origin-A sync may create only its own snapshot receipt");
  expect(originBReceipt.status == ir::IrReceiptReadStatus::Found &&
             originBReceipt.receipt &&
             originBReceipt.receipt->remoteScoreId == "other-origin-score",
         "origin-A sync leaves the origin-B durable receipt unchanged");
  expect(retainedOutbox.status == ir::IrOutboxReadStatus::Found &&
             retainedOutbox.entry &&
             retainedOutbox.entry->id == delivered.entry->id &&
             retainedOutbox.entry->state == ir::IrOutboxState::Succeeded &&
             completed.outboxRowsSettled == 0,
         "origin-A sync does not purge origin-B retained success work");

  harness.driver->pushReconciliation({
      .status = ir::IrUserScoreSnapshotStatus::Succeeded,
      .snapshot = ir::IrUserScoreSnapshot{.scores = {remoteScore()}},
  });
  harness.now.fetch_add(60'000);
  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted &&
             harness.driver->waitForReconciliationCalls(2) &&
             waitForReconciliationPhase(harness,
                                        ir::IrReconciliationPhase::Succeeded),
         "a second origin-A reconciliation completes after the cooldown");

  const auto secondOriginAReceipt =
      harness.repository.LoadIrSubmissionReceipt(
          "fake", "https://boku.tachi.ac", attemptId(36));
  const auto secondOriginBReceipt =
      harness.repository.LoadIrSubmissionReceipt(
          "fake", "https://other.example", attemptId(36));
  const auto secondRetainedOutbox =
      harness.repository.LoadIrOutbox("fake", attemptId(36));
  expect(secondOriginAReceipt.receipt &&
             secondOriginAReceipt.receipt->source ==
                 ir::IrReceiptConfirmationSource::Snapshot &&
             secondOriginBReceipt.receipt &&
             secondOriginBReceipt.receipt->source ==
                 ir::IrReceiptConfirmationSource::Submission &&
             secondRetainedOutbox.entry &&
             secondRetainedOutbox.entry->state ==
                 ir::IrOutboxState::Succeeded,
         "a later snapshot receipt never acquires another origin's delivery "
         "ownership");

  harness.service->stop();
  const auto clearedOriginA = harness.repository.ClearIrAccountEvidence(
      "fake", "https://boku.tachi.ac");
  expect(clearedOriginA.status == ir::IrOutboxMutationStatus::Updated &&
             harness.repository
                     .LoadIrSubmissionReceipt("fake",
                                              "https://boku.tachi.ac",
                                              attemptId(36))
                     .status == ir::IrReceiptReadStatus::NotFound &&
             harness.repository
                     .LoadIrSubmissionReceipt("fake", "https://other.example",
                                              attemptId(36))
                     .status == ir::IrReceiptReadStatus::Found &&
             harness.repository.LoadIrOutbox("fake", attemptId(36)).status ==
                 ir::IrOutboxReadStatus::Found,
         "clearing origin A preserves origin B's receipt and retained row");

  const auto clearedOriginB = harness.repository.ClearIrAccountEvidence(
      "fake", "https://other.example");
  expect(clearedOriginB.status == ir::IrOutboxMutationStatus::Updated &&
             harness.repository
                     .LoadIrSubmissionReceipt("fake", "https://other.example",
                                              attemptId(36))
                     .status == ir::IrReceiptReadStatus::NotFound &&
             harness.repository.LoadIrOutbox("fake", attemptId(36)).status ==
                 ir::IrOutboxReadStatus::NotFound,
         "clearing origin B purges the row owned by its submission receipt");
}

void testRepositoryFailurePublishesFailedAndPreservesPriorSnapshot() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  expect(harness.repository
                 .ApplyIrRemoteSnapshot({
                     .providerId = "fake",
                     .serverOrigin = "https://boku.tachi.ac",
                     .synchronizedAtUnixMillis = harness.now.load(),
                     .scores = {remoteScore("prior-remote-score")},
                 })
                 .status == ir::IrRemoteSnapshotApplyOutcome::Status::Applied,
         "repository-failure fixture seeds its prior snapshot");
  expect(executeSql(
             harness.temp.path() / "replays.db",
             "CREATE TRIGGER fail_service_remote_insert BEFORE INSERT ON "
             "ir_remote_scores BEGIN SELECT RAISE(ABORT,'injected'); END"),
         "repository-failure fixture installs an apply failure");
  harness.driver->pushReconciliation({
      .status = ir::IrUserScoreSnapshotStatus::Succeeded,
      .snapshot = ir::IrUserScoreSnapshot{
          .scores = {remoteScore("replacement-remote-score")}},
  });
  harness.driver->releaseReconciliationStage(2);
  harness.service->start(
      profile(true, true, "https://boku.tachi.ac"));

  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted &&
             waitForReconciliationPhase(harness,
                                        ir::IrReconciliationPhase::Failed),
         "repository failure publishes failed instead of succeeded");
  const auto failed = harness.service->reconciliationStatus("fake");
  const auto mirror = harness.repository.ListIrRemoteScores(
      "fake", "https://boku.tachi.ac");
  expect(!failed.diagnostic.empty() && failed.remoteScores == 0 &&
             failed.nextAllowedAt.has_value(),
         "repository failure publishes bounded failure with cooldown only");
  expect(mirror.status == ir::IrRemoteScoreReadOutcome::Status::Loaded &&
             mirror.scores.size() == 1 &&
             mirror.scores.front().remoteScoreId == "prior-remote-score",
         "failed atomic apply leaves the previous remote mirror unchanged");
}

void testReconciliationRefreshesSettledOutboxSnapshots() {
  Harness harness({.readOnly = false,
                   .chartRankings = false,
                   .scoreSubmission = true,
                   .deferredSubmission = true,
                   .scoreReconciliation = true});
  harness.setCredential("record-sync-key");
  const auto pending =
      harness.enqueueReady(draft(35, harness.now.load() + 60'000), false);
  expect(pending.entry.has_value(),
         "settled-outbox fixture stores future pending work");
  harness.driver->pushReconciliation({
      .status = ir::IrUserScoreSnapshotStatus::Succeeded,
      .snapshot = ir::IrUserScoreSnapshot{.scores = {remoteScore()}},
  });
  harness.driver->releaseReconciliationStage(2);
  harness.service->start(
      profile(true, true, "https://boku.tachi.ac"));
  expect(harness.waiter.waitForEntries(1),
         "settled-outbox fixture worker waits for future delivery");

  expect(harness.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Accepted &&
             waitForReconciliationPhase(harness,
                                        ir::IrReconciliationPhase::Succeeded),
         "remote representation settles matching pending work");
  const auto completed = harness.service->reconciliationStatus("fake");
  expect(completed.outboxRowsSettled == 1 &&
             harness.repository.LoadIrOutbox("fake", attemptId(35)).status ==
                 ir::IrOutboxReadStatus::NotFound,
         "atomic reconciliation removes the represented outbox row");
  expect(harness.service->counts("fake").pending == 0 &&
             !harness.service->status("fake", attemptId(35)).found,
         "service snapshots refresh after reconciliation settles outbox work");
}

void testReconciliationRequestRejectsUnavailableServiceAndConfiguration() {
  Harness unsupported;
  expect(unsupported.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::Unsupported,
         "driver without reconciliation capability is unsupported");

  const ir::IrDriverCapabilities capabilities{
      .readOnly = true,
      .chartRankings = false,
      .scoreSubmission = false,
      .deferredSubmission = false,
      .scoreReconciliation = true,
  };
  Harness inactive(capabilities);
  inactive.setCredential("record-sync-key");
  expect(inactive.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::ServiceInactive,
         "supported reconciliation requires an active service");

  Harness disabled(capabilities);
  disabled.setCredential("record-sync-key");
  disabled.service->start(profile(false, false));
  expect(disabled.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::ConfigurationRequired,
         "disabled provider requires configuration before reconciliation");

  Harness missingCredential(capabilities);
  missingCredential.service->start(profile(true, false));
  expect(missingCredential.service->requestUserScoreReconciliation("fake") ==
             ir::IrReconciliationRequestStatus::ConfigurationRequired,
         "missing credential requires configuration before reconciliation");
}

} // namespace

int main() {
  static_assert(ir::kMaximumAttemptStatusSnapshots > 0);
  testDueAttemptsSubmitAsOneAtomicGroup();
  testMixedRequestKindsUseTwoPlannedCalls();
  testSharedDeferredGroupPollsOnce();
  testGroupedCancellationRecoversEveryClaim();
  testMissingCredentialBlocksWholePlannedGroup();
  testAmbiguousPartialResultFailsWholeGroup();
  testIdentifiedInvalidAndUnsupportedPlansIsolateRejectedRow();
  testIdentifiedLaterPlanRejectionNeverDiscardsTheFirstDueRow();
  testUnidentifiedPlanFailureDoesNotDiscardAnyDueRow();
  testTachiMalformedLaterRowDoesNotDiscardOrStarveValidWork();
  testEchoedCredentialResponseIdentitiesAreNeverPersisted();
  testFailedAtomicDeliveryApplyRequeuesTheWholeClaimedGroup();
  testFailedExactGroupRecoveryNeverResetsUnrelatedUploadingRows();
  testActiveRequestSnapshotsDistinguishSubmitAndPoll();
  testStartupRecovery();
  testDisabledAndReadOnlyProvidersStayPaused();
  testFutureWakeIgnoresBoundedSkippedProviderRows();
  testMissingKeyPreservesManualIntentAndReplacementWakes();
  testProviderRuntimeChangeUnblocksRows();
  testManualEnqueueRequiresFreshRulesetProof();
  testManualBatchPublishesAndWakesOnceWithSingularCompatibility();
  testAutomaticAndManualRequestsUseCurrentOrigin();
  testDeferredPollingPinsOriginAndNeverReposts();
  testAdaptiveDeferredPollingCadence();
  testDeferredInitialPollIgnoresEarlierPostFailures();
  testPersistedBackoffAndRetryAfter();
  testPermanentFailureRetryAllAndDeferredPreservation();
  testSuccessfulDeliveryPersistsRemoteReceiptBeforeStatus();
  testInvalidSuccessfulReceiptIdentityCompletesAsFailure();
  testCredentialMutationWaitsForOldAccountWorkBeforeClearingEvidence();
  testSucceededPurgeAndSnapshotReads();
  testPauseCancelsInflightAndRecoversClaim();
  testForegroundRecoversAbandonedClaim();
  testForegroundPreservesPendingCredentialChange();
  testLifecycleCancellationWinsBeforeRequestStarts();
  testReconciliationPublishesEverySuccessfulWorkerPhase();
  testQueuedReconciliationRejectsAChangedCredentialGeneration();
  testPauseCancelsAQueuedReconciliationBeforeAnyApply();
  testReconciliationCoalescesAndSerializesNewOutboxDelivery();
  testReconciliationUsesExactMonotonicCooldownAfterSuccessAndFailure();
  testProfileAndOriginChangeDropAnInflightSnapshotBeforeApply();
  testReconciliationLoadsPlansAndAppliesOneCompleteSnapshot();
  testProjectionFailurePublishesFailedButKeepsCommittedMirror();
  testEmptyReconciliationStillProjectsDeletionSnapshot();
  testReconciliationPreservesSucceededWorkDeliveredToAnotherOrigin();
  testRepositoryFailurePublishesFailedAndPreservesPriorSnapshot();
  testReconciliationRefreshesSettledOutboxSnapshots();
  testReconciliationRequestRejectsUnavailableServiceAndConfiguration();

  if (failures != 0) {
    std::cerr << failures << " IR submission service test(s) failed\n";
    return 1;
  }
  std::cout << "IR submission service tests passed\n";
  return 0;
}
