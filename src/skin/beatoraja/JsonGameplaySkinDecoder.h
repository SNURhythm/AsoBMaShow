#pragma once

#include "BeatorajaSkinModel.h"
#include "LuaSkinBindingDecoder.h"
#include "../SkinSafetyPolicy.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace skin {

struct JsonGameplaySkinDecoderPolicy {
  static constexpr std::size_t maxDocumentBytes = 8U * 1024U * 1024U;
  static constexpr std::size_t maxDepth = 64;
  static constexpr std::size_t maxValues = 200'000;
  static constexpr std::size_t maxArrayEntries = 8'192;
};

struct JsonGameplaySkinDecodeResult {
  std::optional<BeatorajaSkinHeader> header;
  std::optional<BeatorajaSkinConfiguration> configuration;
  std::optional<EntryProfileSettings> reconciledSettings;
  std::optional<BeatorajaSkinModel> model;
  std::vector<SkinDiagnostic> diagnostics;
};

class JsonGameplaySkinDecoder final {
public:
  [[nodiscard]] JsonGameplaySkinDecodeResult decode(
      std::span<const std::byte> bytes, const SkinEntryId &entry,
      const EntryProfileSettings *desired,
      SkinBuiltinBindingCatalogView builtins,
      SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{}) const;
};

} // namespace skin
