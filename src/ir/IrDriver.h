#pragma once

#include "../ThreadCompat.h"
#include "IrOutboxModels.h"
#include "IrProfileSettings.h"
#include "IrRankingModels.h"
#include "IrRemoteScoreModels.h"
#include "IrSubmission.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

class IrHttpClient;

struct IrDriverCapabilities {
  bool readOnly = false;
  bool chartRankings = false;
  bool scoreSubmission = false;
  bool deferredSubmission = false;
  bool scoreReconciliation = false;

  bool operator==(const IrDriverCapabilities &) const = default;
};

enum class BuildDraftStatus { Built, Unsupported, Invalid };
enum class SubmissionEligibilityReason {
  Eligible,
  UnsupportedKeyMode,
  CourseResult,
  RulesetMismatch,
  UnsupportedRulesetRevision,
  UnverifiedProvenance,
  ModifiedJudgePolicy,
  ModifiedGaugeTotal,
  ModifiedAttempt,
  InvalidSubmission,
};
enum class DeliveryStatus {
  Succeeded,
  Deferred,
  Ongoing,
  TransientFailure,
  BlockedConfiguration,
  PermanentFailure,
  Unsupported,
  Cancelled,
};
enum class IrOutboxBatchPlanStatus { Planned, Invalid, Unsupported };
enum class ChartRankingStatus {
  Succeeded,
  ChartNotFound,
  AuthenticationRequired,
  TransientFailure,
  Unsupported,
  MalformedResponse,
  OversizedResponse,
  Cancelled,
};

struct BuildDraftOutcome {
  BuildDraftStatus status = BuildDraftStatus::Invalid;
  SubmissionEligibilityReason reason =
      SubmissionEligibilityReason::InvalidSubmission;
  std::optional<IrOutboxDraft> draft;
  std::string diagnostic;
};

struct DeliveryOutcome {
  DeliveryStatus status = DeliveryStatus::PermanentFailure;
  std::optional<std::int64_t> remoteUserId;
  std::optional<std::string> remoteScoreId;
  std::vector<std::string> remoteScoreIds;
  bool importHadErrors = false;
  std::optional<std::string> remoteJobId;
  std::optional<std::string> remoteOrigin;
  std::optional<std::chrono::milliseconds> retryAfterDelay;
  std::string code;
  std::string diagnostic;
};

struct IrOutboxBatchPlan {
  IrOutboxBatchPlanStatus status = IrOutboxBatchPlanStatus::Invalid;
  std::vector<std::int64_t> rowIds;
  std::optional<std::int64_t> rejectedRowId;
  std::string diagnostic;
};

struct ChartRankingOutcome {
  ChartRankingStatus status = ChartRankingStatus::MalformedResponse;
  std::optional<IrChartRanking> ranking;
  std::string diagnostic;
};

struct IrProviderRuntimeConfig {
  std::string profileId;
  std::string serverOrigin;
  std::string apiKey;
};

using IrUserScoreProgress =
    std::function<void(std::string_view game, int completed, int total)>;

class IrDriver {
public:
  virtual ~IrDriver() = default;
  virtual std::string_view providerId() const noexcept = 0;
  virtual IrDriverCapabilities capabilities() const noexcept = 0;
  virtual BuildDraftOutcome buildDraft(const IrSubmission &) const;
  virtual IrOutboxBatchPlan planBatch(std::span<const IrOutboxEntry> due) const;
  virtual DeliveryOutcome submit(const IrOutboxEntry &,
                                 const IrProviderRuntimeConfig &,
                                 IrHttpClient &, std::stop_token) const;
  virtual DeliveryOutcome submitBatch(std::span<const IrOutboxEntry> entries,
                                      bool userIntent,
                                      const IrProviderRuntimeConfig &,
                                      IrHttpClient &, std::stop_token) const;
  virtual DeliveryOutcome poll(const IrOutboxEntry &,
                               const IrProviderRuntimeConfig &, IrHttpClient &,
                               std::stop_token) const;
  virtual DeliveryOutcome pollBatch(std::span<const IrOutboxEntry> entries,
                                    const IrProviderRuntimeConfig &,
                                    IrHttpClient &, std::stop_token) const;
  virtual ChartRankingOutcome fetchChartRanking(const IrChartQuery &,
                                                const IrProviderRuntimeConfig &,
                                                IrHttpClient &,
                                                std::stop_token) const;
  virtual ChartRankingOutcome
  fetchChartRankingPage(const IrChartQuery &, std::string_view pageToken,
                        const IrProviderRuntimeConfig &, IrHttpClient &,
                        std::stop_token) const;
  virtual IrUserScoreSnapshotOutcome
  fetchUserScoreSnapshot(const IrProviderRuntimeConfig &, IrHttpClient &,
                         std::stop_token, IrUserScoreProgress) const;
};

[[nodiscard]] bool
validateCapabilities(IrDriverCapabilities capabilities) noexcept;

class IrDriverRegistry {
public:
  bool registerDriver(std::shared_ptr<const IrDriver> driver,
                      std::string &diagnostic);
  [[nodiscard]] std::shared_ptr<const IrDriver>
  find(std::string_view providerId) const;
  [[nodiscard]] BuildDraftOutcome
  buildDraft(std::string_view providerId, const IrSubmission &submission) const;
  [[nodiscard]] IrOutboxBatchPlan
  planBatch(std::string_view providerId,
            std::span<const IrOutboxEntry> due) const;
  [[nodiscard]] DeliveryOutcome submit(std::string_view providerId,
                                       const IrOutboxEntry &entry,
                                       const IrProviderRuntimeConfig &config,
                                       IrHttpClient &http,
                                       std::stop_token stopToken) const;
  [[nodiscard]] DeliveryOutcome
  submitBatch(std::string_view providerId,
              std::span<const IrOutboxEntry> entries, bool userIntent,
              const IrProviderRuntimeConfig &config, IrHttpClient &http,
              std::stop_token stopToken) const;
  [[nodiscard]] DeliveryOutcome poll(std::string_view providerId,
                                     const IrOutboxEntry &entry,
                                     const IrProviderRuntimeConfig &config,
                                     IrHttpClient &http,
                                     std::stop_token stopToken) const;
  [[nodiscard]] DeliveryOutcome
  pollBatch(std::string_view providerId, std::span<const IrOutboxEntry> entries,
            const IrProviderRuntimeConfig &config, IrHttpClient &http,
            std::stop_token stopToken) const;
  [[nodiscard]] ChartRankingOutcome
  fetchChartRanking(std::string_view providerId, const IrChartQuery &query,
                    const IrProviderRuntimeConfig &config, IrHttpClient &http,
                    std::stop_token stopToken) const;
  [[nodiscard]] ChartRankingOutcome
  fetchChartRankingPage(std::string_view providerId, const IrChartQuery &query,
                        std::string_view pageToken,
                        const IrProviderRuntimeConfig &config,
                        IrHttpClient &http, std::stop_token stopToken) const;
  [[nodiscard]] IrUserScoreSnapshotOutcome
  fetchUserScoreSnapshot(std::string_view providerId,
                         const IrProviderRuntimeConfig &config,
                         IrHttpClient &http, std::stop_token stopToken,
                         IrUserScoreProgress progress) const;
  [[nodiscard]] std::vector<IrOutboxDraft> buildAutomaticDrafts(
      const std::map<std::string, IrProviderSettings> &settings,
      const IrSubmission &submission) const;

private:
  std::map<std::string, std::shared_ptr<const IrDriver>, std::less<>> drivers_;
};

} // namespace ir
