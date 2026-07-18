#include "IrDriver.h"

#include <algorithm>
#include <cctype>

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

ChartRankingOutcome unsupportedRanking(std::string_view diagnostic) {
  return {.status = ChartRankingStatus::Unsupported,
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

DeliveryOutcome IrDriver::submit(const IrOutboxEntry &,
                                 const IrProviderRuntimeConfig &,
                                 IrHttpClient &, std::stop_token) const {
  return unsupportedDelivery("driver does not support score submission");
}

DeliveryOutcome IrDriver::poll(const IrOutboxEntry &,
                               const IrProviderRuntimeConfig &, IrHttpClient &,
                               std::stop_token) const {
  return unsupportedDelivery("driver does not support deferred submission");
}

ChartRankingOutcome IrDriver::fetchChartRanking(const IrChartQuery &,
                                                const IrProviderRuntimeConfig &,
                                                IrHttpClient &,
                                                std::stop_token) const {
  return unsupportedRanking("driver does not support chart rankings");
}

bool validateCapabilities(IrDriverCapabilities capabilities) noexcept {
  return (capabilities.chartRankings || capabilities.scoreSubmission) &&
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
