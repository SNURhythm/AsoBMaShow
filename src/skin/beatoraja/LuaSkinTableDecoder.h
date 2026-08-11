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
  // Full gameplay-model decoding uses a bounded copy budget. Header and
  // configuration declarations intentionally do not use this budget because
  // Beatoraja stores their authored strings without a fixed text limit.
  static constexpr std::size_t maxGameplayTextBytes = 8 * 1024 * 1024;
  static constexpr int maxGameplayDimension = 8'192;
  static constexpr std::size_t maxGameplayOffsets = 256;
};

class LuaSkinTableDecoder final {
public:
  HeaderDecodeResult decodeHeader(const LuaValueHandle &) const;
  BeatorajaSkinModelDecodeResult
  decodeGameplay(const LuaValueHandle &, LuaSkinGameplayDecodeContext) const;
};

ConfigurationReconcileResult
reconcileSkinConfiguration(const BeatorajaSkinHeader &,
                           const EntryProfileSettings *, LuaSkinFileSystem &,
                           const RuntimeSkinConfigurationSelection * = nullptr);

} // namespace skin
