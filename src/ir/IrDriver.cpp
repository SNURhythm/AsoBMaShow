#include "IrDriver.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace ir {
namespace {

BuildDraftOutcome unsupportedBuild(std::string_view diagnostic) {
  return {.status = BuildDraftStatus::Unsupported,
          .reason = SubmissionEligibilityReason::InvalidSubmission,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

DeliveryOutcome unsupportedDelivery(std::string_view diagnostic) {
  return {.status = DeliveryStatus::Unsupported,
          .code = "unsupported_operation",
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

DeliveryOutcome invalidBatchDelivery(std::string_view diagnostic) {
  return {.status = DeliveryStatus::PermanentFailure,
          .code = "invalid_batch",
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

IrOutboxBatchPlan invalidBatchPlan(std::string_view diagnostic) {
  return {.status = IrOutboxBatchPlanStatus::Invalid,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

IrOutboxBatchPlan unsupportedBatchPlan(std::string_view diagnostic) {
  return {.status = IrOutboxBatchPlanStatus::Unsupported,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

ChartRankingOutcome unsupportedRanking(std::string_view diagnostic) {
  return {.status = ChartRankingStatus::Unsupported,
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

IrUserScoreSnapshotOutcome
unsupportedReconciliation(std::string_view diagnostic) {
  return {.status = IrUserScoreSnapshotStatus::Unsupported,
          .code = "unsupported_operation",
          .diagnostic = sanitizeDiagnostic(diagnostic)};
}

bool validProviderId(std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '-' ||
                  character == '_' || character == '.';
         });
}

} // namespace

BuildDraftOutcome IrDriver::buildDraft(const IrSubmission &) const {
  return unsupportedBuild("driver does not support score submission");
}

IrOutboxBatchPlan
IrDriver::planBatch(std::span<const IrOutboxEntry> due) const {
  const auto first = std::ranges::find_if(
      due, [](const IrOutboxEntry &entry) { return entry.id > 0; });
  if (first == due.end()) {
    return invalidBatchPlan("IR batch has no valid due row");
  }
  return {.status = IrOutboxBatchPlanStatus::Planned,
          .rowIds = {first->id}};
}

DeliveryOutcome IrDriver::submit(const IrOutboxEntry &,
                                 const IrProviderRuntimeConfig &,
                                 IrHttpClient &, std::stop_token) const {
  return unsupportedDelivery("driver does not support score submission");
}

DeliveryOutcome IrDriver::submitBatch(std::span<const IrOutboxEntry> entries,
                                      bool userIntent,
                                      const IrProviderRuntimeConfig &config,
                                      IrHttpClient &http,
                                      std::stop_token stopToken) const {
  if (entries.size() != 1) {
    return invalidBatchDelivery(
        "IR driver singular submission fallback requires exactly one row");
  }
  IrOutboxEntry entry = entries.front();
  entry.nextRequestUserIntent = userIntent;
  return submit(entry, config, http, stopToken);
}

DeliveryOutcome IrDriver::poll(const IrOutboxEntry &,
                               const IrProviderRuntimeConfig &, IrHttpClient &,
                               std::stop_token) const {
  return unsupportedDelivery("driver does not support deferred submission");
}

DeliveryOutcome IrDriver::pollBatch(std::span<const IrOutboxEntry> entries,
                                    const IrProviderRuntimeConfig &config,
                                    IrHttpClient &http,
                                    std::stop_token stopToken) const {
  if (entries.size() != 1) {
    return invalidBatchDelivery(
        "IR driver singular polling fallback requires exactly one row");
  }
  return poll(entries.front(), config, http, stopToken);
}

ChartRankingOutcome IrDriver::fetchChartRanking(const IrChartQuery &,
                                                const IrProviderRuntimeConfig &,
                                                IrHttpClient &,
                                                std::stop_token) const {
  return unsupportedRanking("driver does not support chart rankings");
}

ChartRankingOutcome
IrDriver::fetchChartRankingPage(const IrChartQuery &, std::string_view,
                                const IrProviderRuntimeConfig &, IrHttpClient &,
                                std::stop_token) const {
  return unsupportedRanking("driver does not support paged chart rankings");
}

IrUserScoreSnapshotOutcome
IrDriver::fetchUserScoreSnapshot(const IrProviderRuntimeConfig &,
                                 IrHttpClient &, std::stop_token,
                                 IrUserScoreProgress) const {
  return unsupportedReconciliation(
      "driver does not support score reconciliation");
}

bool validateCapabilities(IrDriverCapabilities capabilities) noexcept {
  return (capabilities.chartRankings || capabilities.scoreSubmission ||
          capabilities.scoreReconciliation) &&
         (!capabilities.readOnly || (!capabilities.scoreSubmission &&
                                     !capabilities.deferredSubmission)) &&
         (!capabilities.deferredSubmission || capabilities.scoreSubmission);
}

bool IrDriverRegistry::registerDriver(std::shared_ptr<const IrDriver> driver,
                                      std::string &diagnostic) {
  diagnostic.clear();
  if (!driver) {
    diagnostic = "IR driver is missing";
    return false;
  }
  const std::string providerId(driver->providerId());
  if (!validProviderId(providerId)) {
    diagnostic = "IR driver provider ID is invalid";
    return false;
  }
  if (!validateCapabilities(driver->capabilities())) {
    diagnostic = "IR driver capability declaration is contradictory";
    return false;
  }
  if (drivers_.contains(providerId)) {
    diagnostic = "IR driver provider ID is already registered";
    return false;
  }
  drivers_.emplace(providerId, std::move(driver));
  return true;
}

std::shared_ptr<const IrDriver>
IrDriverRegistry::find(std::string_view providerId) const {
  const auto found = drivers_.find(providerId);
  return found == drivers_.end() ? nullptr : found->second;
}

BuildDraftOutcome
IrDriverRegistry::buildDraft(std::string_view providerId,
                             const IrSubmission &submission) const {
  const auto driver = find(providerId);
  if (!driver) {
    return unsupportedBuild("IR provider is not registered");
  }
  const auto capabilities = driver->capabilities();
  if (capabilities.readOnly || !capabilities.scoreSubmission) {
    return unsupportedBuild("IR provider is read-only");
  }
  try {
    return driver->buildDraft(submission);
  } catch (...) {
    return {.status = BuildDraftStatus::Invalid,
            .reason = SubmissionEligibilityReason::InvalidSubmission,
            .diagnostic = "IR draft construction failed"};
  }
}

IrOutboxBatchPlan
IrDriverRegistry::planBatch(std::string_view providerId,
                            std::span<const IrOutboxEntry> due) const {
  const auto driver = find(providerId);
  if (!driver || driver->capabilities().readOnly ||
      !driver->capabilities().scoreSubmission) {
    return unsupportedBatchPlan("IR provider cannot submit scores");
  }
  try {
    IrOutboxBatchPlan plan = driver->planBatch(due);
    if (plan.status != IrOutboxBatchPlanStatus::Planned) {
      plan.rowIds.clear();
      plan.diagnostic = sanitizeDiagnostic(plan.diagnostic);
      return plan;
    }
    if (plan.rowIds.empty() || plan.rowIds.size() > 64) {
      return invalidBatchPlan("IR driver returned an invalid batch size");
    }
    std::unordered_set<std::int64_t> dueIds;
    dueIds.reserve(due.size());
    for (const auto &entry : due) {
      dueIds.insert(entry.id);
    }
    std::unordered_set<std::int64_t> plannedIds;
    plannedIds.reserve(plan.rowIds.size());
    for (const std::int64_t rowId : plan.rowIds) {
      if (rowId <= 0 || !dueIds.contains(rowId) ||
          !plannedIds.insert(rowId).second) {
        return invalidBatchPlan(
            "IR driver returned duplicate or unknown batch row IDs");
      }
    }
    plan.diagnostic = sanitizeDiagnostic(plan.diagnostic);
    return plan;
  } catch (...) {
    return invalidBatchPlan("IR batch planning driver failed");
  }
}

DeliveryOutcome IrDriverRegistry::submit(std::string_view providerId,
                                         const IrOutboxEntry &entry,
                                         const IrProviderRuntimeConfig &config,
                                         IrHttpClient &http,
                                         std::stop_token stopToken) const {
  const auto driver = find(providerId);
  if (!driver || driver->capabilities().readOnly ||
      !driver->capabilities().scoreSubmission) {
    return unsupportedDelivery("IR provider cannot submit scores");
  }
  try {
    return driver->submit(entry, config, http, stopToken);
  } catch (...) {
    return {.status = DeliveryStatus::TransientFailure,
            .code = "driver_exception",
            .diagnostic = "IR submission driver failed"};
  }
}

DeliveryOutcome IrDriverRegistry::submitBatch(
    std::string_view providerId, std::span<const IrOutboxEntry> entries,
    bool userIntent, const IrProviderRuntimeConfig &config, IrHttpClient &http,
    std::stop_token stopToken) const {
  const auto driver = find(providerId);
  if (!driver || driver->capabilities().readOnly ||
      !driver->capabilities().scoreSubmission) {
    return unsupportedDelivery("IR provider cannot submit scores");
  }
  try {
    return driver->submitBatch(entries, userIntent, config, http, stopToken);
  } catch (...) {
    return {.status = DeliveryStatus::TransientFailure,
            .code = "driver_exception",
            .diagnostic = "IR batch submission driver failed"};
  }
}

DeliveryOutcome IrDriverRegistry::poll(std::string_view providerId,
                                       const IrOutboxEntry &entry,
                                       const IrProviderRuntimeConfig &config,
                                       IrHttpClient &http,
                                       std::stop_token stopToken) const {
  const auto driver = find(providerId);
  if (!driver || driver->capabilities().readOnly ||
      !driver->capabilities().scoreSubmission ||
      !driver->capabilities().deferredSubmission) {
    return unsupportedDelivery("IR provider cannot poll submissions");
  }
  try {
    return driver->poll(entry, config, http, stopToken);
  } catch (...) {
    return {.status = DeliveryStatus::TransientFailure,
            .code = "driver_exception",
            .diagnostic = "IR polling driver failed"};
  }
}

DeliveryOutcome IrDriverRegistry::pollBatch(
    std::string_view providerId, std::span<const IrOutboxEntry> entries,
    const IrProviderRuntimeConfig &config, IrHttpClient &http,
    std::stop_token stopToken) const {
  const auto driver = find(providerId);
  if (!driver || driver->capabilities().readOnly ||
      !driver->capabilities().scoreSubmission ||
      !driver->capabilities().deferredSubmission) {
    return unsupportedDelivery("IR provider cannot poll submissions");
  }
  try {
    return driver->pollBatch(entries, config, http, stopToken);
  } catch (...) {
    return {.status = DeliveryStatus::TransientFailure,
            .code = "driver_exception",
            .diagnostic = "IR batch polling driver failed"};
  }
}

ChartRankingOutcome IrDriverRegistry::fetchChartRanking(
    std::string_view providerId, const IrChartQuery &query,
    const IrProviderRuntimeConfig &config, IrHttpClient &http,
    std::stop_token stopToken) const {
  const auto driver = find(providerId);
  if (!driver || !driver->capabilities().chartRankings) {
    return unsupportedRanking("IR provider cannot read chart rankings");
  }
  try {
    return driver->fetchChartRanking(query, config, http, stopToken);
  } catch (...) {
    return {.status = ChartRankingStatus::TransientFailure,
            .diagnostic = "IR ranking driver failed"};
  }
}

ChartRankingOutcome IrDriverRegistry::fetchChartRankingPage(
    std::string_view providerId, const IrChartQuery &query,
    std::string_view pageToken, const IrProviderRuntimeConfig &config,
    IrHttpClient &http, std::stop_token stopToken) const {
  const auto driver = find(providerId);
  if (!driver || !driver->capabilities().chartRankings) {
    return unsupportedRanking("IR provider cannot read chart rankings");
  }
  try {
    return driver->fetchChartRankingPage(query, pageToken, config, http,
                                         stopToken);
  } catch (...) {
    return {.status = ChartRankingStatus::TransientFailure,
            .diagnostic = "IR ranking page driver failed"};
  }
}

IrUserScoreSnapshotOutcome IrDriverRegistry::fetchUserScoreSnapshot(
    std::string_view providerId, const IrProviderRuntimeConfig &config,
    IrHttpClient &http, std::stop_token stopToken,
    IrUserScoreProgress progress) const {
  const auto driver = find(providerId);
  if (!driver || !driver->capabilities().scoreReconciliation) {
    return unsupportedReconciliation(
        "IR provider cannot reconcile user scores");
  }
  try {
    return driver->fetchUserScoreSnapshot(config, http, stopToken,
                                          std::move(progress));
  } catch (...) {
    return {.status = IrUserScoreSnapshotStatus::TransientFailure,
            .code = "driver_exception",
            .diagnostic = "IR reconciliation driver failed"};
  }
}

std::vector<IrOutboxDraft> IrDriverRegistry::buildAutomaticDrafts(
    const std::map<std::string, IrProviderSettings> &settings,
    const IrSubmission &submission) const {
  std::vector<IrOutboxDraft> drafts;
  drafts.reserve(settings.size());
  for (const auto &[providerId, providerSettings] : settings) {
    if (!providerSettings.enabled || !providerSettings.autoSubmit) {
      continue;
    }
    const auto driver = find(providerId);
    if (!driver) {
      continue;
    }
    const auto capabilities = driver->capabilities();
    if (capabilities.readOnly || !capabilities.scoreSubmission) {
      continue;
    }
    BuildDraftOutcome built = buildDraft(providerId, submission);
    if (built.status != BuildDraftStatus::Built || !built.draft) {
      continue;
    }
    std::string diagnostic;
    if (!validateIrOutboxDraft(*built.draft, diagnostic) ||
        built.draft->providerId != providerId ||
        built.draft->attemptId != submission.attemptId ||
        built.draft->chartMd5 != submission.chartMd5 ||
        built.draft->chartSha256 != submission.chartSha256 ||
        built.draft->createdAtUnixMillis != submission.playedAtUnixMillis) {
      continue;
    }
    drafts.push_back(std::move(*built.draft));
  }
  return drafts;
}

} // namespace ir
