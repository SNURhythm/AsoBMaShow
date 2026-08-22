#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace skin {

enum class GameplaySkinSourceFormat : std::uint8_t { Lua, Json, Lr2 };

[[nodiscard]] std::optional<GameplaySkinSourceFormat>
gameplaySkinSourceFormatForPath(std::string_view packageRelativePath) noexcept;

} // namespace skin
