#pragma once

#include "BeatorajaSkinModel.h"
#include "Lr2SkinCsvParser.h"
#include "LuaSkinBindingDecoder.h"
#include "StaticSkinDecodeControl.h"
#include "../SkinSafetyPolicy.h"

#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace skin {

struct Lr2GameplaySkinDecodeResult {
  std::optional<BeatorajaSkinConfiguration> configuration;
  std::optional<EntryProfileSettings> reconciledSettings;
  std::optional<BeatorajaSkinModel> model;
  std::vector<SkinDiagnostic> diagnostics;
  bool fatal = false;
  bool cancelled = false;
};

struct Lr2GameplaySkinConfigurationResult {
  std::optional<BeatorajaSkinConfiguration> configuration;
  std::optional<EntryProfileSettings> reconciledSettings;
  std::vector<SkinDiagnostic> diagnostics;
  bool fatal = false;
};

[[nodiscard]] Lr2GameplaySkinConfigurationResult
reconcileLr2GameplaySkinConfiguration(const BeatorajaSkinHeader &,
                                      const EntryProfileSettings *);

class Lr2GameplaySkinDecoder final {
public:
  [[nodiscard]] Lr2GameplaySkinDecodeResult decode(
      const BeatorajaSkinHeader &, std::span<const Lr2SkinCommand>,
      const EntryProfileSettings *, SkinBuiltinBindingCatalogView,
      SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{},
      std::stop_token stop = {},
      StaticSkinDecodeCheckpoint checkpoint = {},
      const std::map<int, int> *initialOptionStates = nullptr) const;
};

} // namespace skin
