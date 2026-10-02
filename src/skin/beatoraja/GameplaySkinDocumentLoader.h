#pragma once

#include "BeatorajaSkinModel.h"
#include "GameplaySkinSourceFormat.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinHttpClient.h"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

class LuaSkinAudioBackend;

// Invoke exactly once, synchronously, while the initial frame is bound. Model
// decoding can execute callback factories and needs the same state as the entry.
using LuaConfiguredGameplayDocumentContinuation = std::function<LuaValueResult()>;
using LuaConfiguredGameplayDocumentLoad = std::function<LuaValueResult(
    LuaSkinRuntime &, const BeatorajaSkinConfiguration &,
    std::vector<SkinDiagnostic> &,
    const LuaConfiguredGameplayDocumentContinuation &)>;

struct GameplaySkinDocumentRequest {
  GameplaySkinSourceFormat sourceFormat = GameplaySkinSourceFormat::Lua;
  SkinEntryId entry;
  LuaSkinFileSystem &documentFileSystem;
  // Required only for Lua. Static formats must leave this null so dispatch
  // cannot accidentally create a Lua VM for JSON or LR2 documents.
  std::unique_ptr<LuaSkinFileSystem> luaFileSystem;
  std::unique_ptr<LuaSkinHttpTransport> luaHttpTransport;
  std::shared_ptr<LuaSkinAudioBackend> luaAudioBackend;
  const EntryProfileSettings *desiredSettings = nullptr;
  const RuntimeSkinConfigurationSelection *pinnedRuntimeSelection = nullptr;
  // When nonempty, a fresh reconciliation must match this already validated
  // identity before configured Lua is allowed to run.
  std::string_view expectedConfigurationDigest;
  LuaRuntimePurpose luaPurpose = LuaRuntimePurpose::Gameplay;
  // Session-only state binding for modules evaluated by the entry's first require.
  // Catalog inspection deliberately ignores this callback.
  std::function<LuaValueResult(LuaSkinRuntime &)> loadHeaderLua;
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

  // Full session preparation. Callbacks bind the caller's initial authority
  // during header/configured execution and Lua model decoding; static formats
  // ignore them.
  [[nodiscard]] GameplaySkinDocumentLoadResult
  load(GameplaySkinDocumentRequest) const;
};

} // namespace skin
