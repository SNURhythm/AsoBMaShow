#pragma once

#include "BeatorajaSkinModel.h"
#include "GameplaySkinSourceFormat.h"
#include "LuaSkinFileSystem.h"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

using LuaConfiguredGameplayDocumentLoad = std::function<LuaValueResult(
    LuaSkinRuntime &, const BeatorajaSkinConfiguration &,
    std::vector<SkinDiagnostic> &)>;

struct GameplaySkinDocumentRequest {
  GameplaySkinSourceFormat sourceFormat = GameplaySkinSourceFormat::Lua;
  SkinEntryId entry;
  LuaSkinFileSystem &documentFileSystem;
  // Required only for Lua. Static formats must leave this null so dispatch
  // cannot accidentally create a Lua VM for JSON or LR2 documents.
  std::unique_ptr<LuaSkinFileSystem> luaFileSystem;
  const EntryProfileSettings *desiredSettings = nullptr;
  const RuntimeSkinConfigurationSelection *pinnedRuntimeSelection = nullptr;
  // When nonempty, a fresh reconciliation must match this already validated
  // identity before configured Lua is allowed to run.
  std::string_view expectedConfigurationDigest;
  LuaRuntimePurpose luaPurpose = LuaRuntimePurpose::Gameplay;
  LuaConfiguredGameplayDocumentLoad loadConfiguredLua;
  SkinSafetyPolicy safetyPolicy{};
  std::stop_token stop;
};

struct InspectedGameplaySkinDocument {
  std::optional<BeatorajaSkinHeader> header;
  std::optional<BeatorajaSkinConfiguration> configuration;
  std::optional<EntryProfileSettings> reconciledSettings;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

struct LoadedGameplaySkinDocument {
  BeatorajaSkinHeader header;
  BeatorajaSkinConfiguration configuration;
  EntryProfileSettings reconciledSettings;
  ValidatedBeatorajaSkinModel model;
  std::unique_ptr<LuaSkinRuntime> luaRuntime;
  std::vector<SkinDiagnostic> diagnostics;
  GameplaySkinSourceFormat sourceFormat = GameplaySkinSourceFormat::Lua;
  SkinEntryId entry;
};

struct GameplaySkinDocumentLoadResult {
  std::optional<LoadedGameplaySkinDocument> document;
  EntryProfileSettings reconciledSettings;
  std::string configurationDigest;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

class GameplaySkinDocumentLoader final {
public:
  // Catalog inspection never executes configured Lua. Static decoders may
  // parse their complete value-owned document, but only header/configuration
  // output participates in discovery admission.
  [[nodiscard]] InspectedGameplaySkinDocument
  inspect(GameplaySkinDocumentRequest) const;

  // Full session preparation. The Lua callback binds the caller's initial
  // authoritative frame around loadConfigured; static formats ignore it.
  [[nodiscard]] GameplaySkinDocumentLoadResult
  load(GameplaySkinDocumentRequest) const;
};

} // namespace skin
