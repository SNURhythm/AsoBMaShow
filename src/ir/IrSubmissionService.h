#pragma once

#include "IrDriver.h"
#include "IrProfileSettings.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ReplayRepository;

namespace ir {

inline constexpr std::size_t kMaximumAttemptStatusSnapshots = 1024;

struct IrActiveProfileConfig {
  std::string profileId;
  std::map<std::string, IrProviderSettings, std::less<>> providers;
};

struct IrAttemptStatusSnapshot {
  std::uint64_t revision = 0;
  bool found = false;
  std::int64_t rowId = 0;
  IrOutboxState state = IrOutboxState::Pending;
  int requestAttemptCount = 0;
  int consecutiveFailureCount = 0;
  std::optional<std::int64_t> nextAttemptAtUnixMillis;
  std::string errorCode;
  std::string diagnostic;

  bool operator==(const IrAttemptStatusSnapshot &) const = default;
};

struct IrSubmissionServiceOptions {
  using SteadyTimePoint = std::chrono::steady_clock::time_point;

  std::function<std::int64_t()> wallNowUnixMillis;
  std::function<SteadyTimePoint()> monotonicNow;
  std::function<std::string(std::string_view profileId,
                            std::string_view providerId)>
      credentialLookup;
  std::function<void(std::stop_token, std::optional<SteadyTimePoint> deadline)>
      waitUntil;
  std::function<void()> wake;
  std::function<void(std::string_view profileId, std::string_view providerId,
                     std::string_view requestOrigin, std::string_view chartMd5,
                     std::string_view chartSha256)>
      submissionSucceeded;
  std::function<void(std::string_view profileId, std::string_view providerId)>
      credentialChanged;
};

class IrSubmissionService {
public:
  IrSubmissionService(ReplayRepository &repository,
                      const IrDriverRegistry &drivers, IrHttpClient &http,
                      IrSubmissionServiceOptions options = {});
  ~IrSubmissionService();

  IrSubmissionService(const IrSubmissionService &) = delete;
  IrSubmissionService &operator=(const IrSubmissionService &) = delete;

  void start(IrActiveProfileConfig config);
  void pauseAndCancel();
  void activateProfile(IrActiveProfileConfig config);
  void setApplicationActive(bool active);
  void notifyConfigurationChanged();
  void notifyOutboxChanged();
  void stop();

  [[nodiscard]] IrOutboxInsertOutcome enqueueManual(const IrOutboxDraft &draft);
  [[nodiscard]] IrOutboxMutationOutcome retry(std::int64_t rowId);
  [[nodiscard]] IrOutboxMutationOutcome retryAll(std::string_view providerId);
  [[nodiscard]] IrOutboxMutationOutcome discard(std::int64_t rowId);
  [[nodiscard]] IrOutboxCounts counts(std::string_view providerId) const;
  [[nodiscard]] std::vector<IrAttemptStatusSnapshot>
  statuses(std::string_view providerId) const;
  [[nodiscard]] IrAttemptStatusSnapshot
  status(std::string_view providerId, std::string_view attemptId) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ir
