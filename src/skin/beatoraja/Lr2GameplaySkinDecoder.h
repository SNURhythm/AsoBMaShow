#pragma once

#include "BeatorajaSkinModel.h"
#include "Lr2SkinCsvParser.h"
#include "LuaSkinBindingDecoder.h"
#include "../SkinSafetyPolicy.h"

#include <optional>
#include <span>
#include <vector>

namespace skin {

struct Lr2GameplaySkinDecodeResult {
  std::optional<BeatorajaSkinConfiguration> configuration;
  std::optional<EntryProfileSettings> reconciledSettings;
  std::optional<BeatorajaSkinModel> model;
  std::vector<SkinDiagnostic> diagnostics;
};

class Lr2GameplaySkinDecoder final {
public:
  [[nodiscard]] Lr2GameplaySkinDecodeResult decode(
      const BeatorajaSkinHeader &, std::span<const Lr2SkinCommand>,
      const EntryProfileSettings *, SkinBuiltinBindingCatalogView,
      SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{}) const;
};

} // namespace skin
