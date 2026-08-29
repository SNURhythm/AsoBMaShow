#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace skin {

using BeatorajaNamedIntegerValueProperty =
    std::pair<std::string_view, int>;

// Pinned from IntegerPropertyFactory.ValueType. These names are deliberately
// shared by the catalog and both state bridges: Beatoraja has independent
// property namespaces, so names must not be resolved through FloatProperty
// or StringProperty IDs.
[[nodiscard]] std::span<const BeatorajaNamedIntegerValueProperty>
beatorajaIntegerValueProperties() noexcept;

[[nodiscard]] std::optional<int>
beatorajaIntegerValuePropertySelector(std::string_view name) noexcept;

using BeatorajaNamedImageIndexProperty = std::pair<std::string_view, int>;

// Pinned from IntegerPropertyFactory.IndexType. This is intentionally a
// separate namespace: the same numeric IDs can mean unrelated ValueType
// properties.
[[nodiscard]] std::span<const BeatorajaNamedImageIndexProperty>
beatorajaImageIndexProperties() noexcept;

[[nodiscard]] std::optional<int>
beatorajaImageIndexPropertySelector(std::string_view name) noexcept;

} // namespace skin
