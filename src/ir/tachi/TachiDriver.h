#pragma once

#include "../IrDriver.h"

#include <memory>

namespace ir::tachi {

class BokutachiCacheStore;

class TachiDriver final : public IrDriver {
public:
  TachiDriver() = default;
  explicit TachiDriver(
      std::shared_ptr<BokutachiCacheStore> cacheStore) noexcept;

  [[nodiscard]] std::string_view providerId() const noexcept override;
  [[nodiscard]] IrDriverCapabilities capabilities() const noexcept override;
  [[nodiscard]] BuildDraftOutcome
  buildDraft(const IrSubmission &submission) const override;
  [[nodiscard]] IrOutboxBatchPlan
  planBatch(std::span<const IrOutboxEntry> due) const override;
  [[nodiscard]] DeliveryOutcome
  submit(const IrOutboxEntry &entry, const IrProviderRuntimeConfig &config,
         IrHttpClient &http, std::stop_token stopToken) const override;
  [[nodiscard]] DeliveryOutcome
  submitBatch(std::span<const IrOutboxEntry> entries, bool userIntent,
              const IrProviderRuntimeConfig &config, IrHttpClient &http,
              std::stop_token stopToken) const override;
  [[nodiscard]] DeliveryOutcome poll(const IrOutboxEntry &entry,
                                     const IrProviderRuntimeConfig &config,
                                     IrHttpClient &http,
                                     std::stop_token stopToken) const override;
  [[nodiscard]] DeliveryOutcome
  pollBatch(std::span<const IrOutboxEntry> entries,
            const IrProviderRuntimeConfig &config, IrHttpClient &http,
            std::stop_token stopToken) const override;
  [[nodiscard]] ChartRankingOutcome
  fetchChartRanking(const IrChartQuery &query,
                    const IrProviderRuntimeConfig &config, IrHttpClient &http,
                    std::stop_token stopToken) const override;
  [[nodiscard]] ChartRankingOutcome
  fetchChartRankingPage(const IrChartQuery &query, std::string_view pageToken,
                        const IrProviderRuntimeConfig &config,
                        IrHttpClient &http,
                        std::stop_token stopToken) const override;
  [[nodiscard]] IrUserScoreSnapshotOutcome
  fetchUserScoreSnapshot(const IrProviderRuntimeConfig &config,
                         IrHttpClient &http, std::stop_token stopToken,
                         IrUserScoreProgress progress) const override;

private:
  std::shared_ptr<BokutachiCacheStore> cacheStore_;
};

} // namespace ir::tachi
