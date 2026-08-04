#pragma once

#include "../package/SkinPackageStore.h"

namespace skin {

class SkinResourcePreparationService;

class GameplaySkinValidator final : public SkinEntryValidator {
public:
  explicit GameplaySkinValidator(SkinResourcePreparationService &) noexcept;

  SkinValidationResult validate(SkinRevisionReadView, const SkinEntryId &,
                                const EntryProfileSettings *,
                                std::stop_token) override;

private:
  SkinResourcePreparationService *resources_ = nullptr;
};

} // namespace skin
