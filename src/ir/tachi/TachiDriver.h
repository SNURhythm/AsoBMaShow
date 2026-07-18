#pragma once

#include "../IrDriver.h"

namespace ir::tachi {

class TachiDriver final : public IrDriver {
public:
  [[nodiscard]] std::string_view providerId() const noexcept override;
  [[nodiscard]] IrDriverCapabilities capabilities() const noexcept override;
  [[nodiscard]] BuildDraftOutcome
  buildDraft(const IrSubmission &submission) const override;
  [[nodiscard]] DeliveryOutcome
  submit(const IrOutboxEntry &entry, const IrProviderRuntimeConfig &config,
         IrHttpClient &http, std::stop_token stopToken) const override;
  [[nodiscard]] DeliveryOutcome poll(const IrOutboxEntry &entry,
                                     const IrProviderRuntimeConfig &config,
                                     IrHttpClient &http,
                                     std::stop_token stopToken) const override;
};

} // namespace ir::tachi
