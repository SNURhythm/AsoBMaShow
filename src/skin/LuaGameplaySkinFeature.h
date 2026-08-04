#pragma once

#ifndef ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#if defined(__ANDROID__)
#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 0
#else
#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1
#endif
#endif

namespace skin {

[[nodiscard]] bool luaGameplaySkinsAvailable() noexcept;

} // namespace skin
