#pragma once

#include "BeatorajaSkinModel.h"
#include "LuaSkinBindingDecoder.h"

#include <cstddef>

namespace skin {

class LuaSkinFileSystem;
class LuaValueHandle;

struct LuaSkinGameplayDecodeContext {
  LuaSkinRuntime &runtime;
  SkinBuiltinBindingCatalogView builtins;
};

struct LuaSkinTableDecoderPolicy {
  static constexpr std::size_t maxDepth = 64;
  static constexpr std::size_t maxEntries = 200'000;
  static constexpr std::size_t maxMaterializedSpriteFrames = 200'000;
  static constexpr std::size_t maxDecodedObjects = 8'192;
  static constexpr std::size_t maxCopiedTextBytes = 8 * 1024 * 1024;
  static constexpr std::size_t maxCategories = 256;
  static constexpr std::size_t maxOptions = 256;
  static constexpr std::size_t maxFiles = 256;
  static constexpr std::size_t maxOffsets = 256;
  static constexpr std::size_t maxCategoryItems = 256;
  static constexpr std::size_t maxOptionChoices = 256;
  // Beatoraja's Lua serializer does not impose a separate small metadata
  // limit. Keep header strings inside the audited Lua copy budget rather than
  // rejecting otherwise valid installed skins at catalog time.
  static constexpr std::size_t maxHeaderTextBytes = maxCopiedTextBytes;
  static constexpr int maxAuthoredDimension = 8'192;
};

class LuaSkinTableDecoder final {
public:
  HeaderDecodeResult decodeHeader(const LuaValueHandle &) const;
  BeatorajaSkinModelDecodeResult
  decodeGameplay(const LuaValueHandle &, LuaSkinGameplayDecodeContext) const;
};

ConfigurationReconcileResult
reconcileSkinConfiguration(const BeatorajaSkinHeader &,
                           const EntryProfileSettings *, LuaSkinFileSystem &);

} // namespace skin
