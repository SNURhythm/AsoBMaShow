#pragma once

#include "LuaSkinBindingDecoder.h"

namespace skin {

// The authoritative built-in surface that PlaySkinStateBridge can currently
// execute. Keep this catalog narrower than Beatoraja's upstream factories:
// admission means the runtime bridge can provide the selected value or event.
[[nodiscard]] SkinBuiltinBindingCatalogView gameplaySkinBuiltinCatalog();

} // namespace skin
