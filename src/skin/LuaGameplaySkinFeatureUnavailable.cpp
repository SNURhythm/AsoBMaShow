#include "LuaGameplaySkinFeature.h"

#if !ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

namespace skin {

bool luaGameplaySkinsAvailable() noexcept { return false; }

} // namespace skin

#endif
