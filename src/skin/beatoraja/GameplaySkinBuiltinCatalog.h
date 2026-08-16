#pragma once

#include "LuaSkinBindingDecoder.h"

namespace skin {

// Numeric IDs accepted by Beatoraja's pinned BooleanPropertyFactory. The
// runtime uses this to distinguish an official but currently inactive feature
// from an unknown selector, which must remain a Lua error.
[[nodiscard]] bool isPinnedBeatorajaBooleanPropertyId(int selector) noexcept;

// The gameplay built-in surface. Every official BooleanPropertyFactory numeric
// selector is admitted; PlaySkinStateBridge preserves exact available state and
// provides Beatoraja's inactive result for gameplay features AsoBMaShow lacks.
[[nodiscard]] SkinBuiltinBindingCatalogView gameplaySkinBuiltinCatalog();

} // namespace skin
