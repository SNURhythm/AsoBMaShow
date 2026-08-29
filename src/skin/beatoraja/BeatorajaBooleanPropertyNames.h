#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace skin {

using BeatorajaNamedBooleanProperty = std::pair<std::string_view, int>;

// Pinned from BooleanPropertyFactory.BooleanType. Boolean properties have an
// independent name namespace: do not resolve these through Integer, Float,
// or String property aliases.
[[nodiscard]] std::span<const BeatorajaNamedBooleanProperty>
beatorajaBooleanProperties() noexcept;

// Mirrors BooleanPropertyFactory.getBooleanProperty(String), including its
// recursive ! prefix and the practice_item{1..16}[_selected] patterns. A
// negative returned value represents a valid source-level negation.
[[nodiscard]] std::optional<int>
beatorajaBooleanPropertySelector(std::string_view name) noexcept;

} // namespace skin
