#pragma once

#include <string>
#include <string_view>

namespace uuid {

[[nodiscard]] std::string generateV4();
[[nodiscard]] bool isStructurallyValid(std::string_view value) noexcept;
[[nodiscard]] bool isCanonicalLowerV4(std::string_view value) noexcept;

} // namespace uuid
