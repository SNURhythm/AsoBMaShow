#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

[[nodiscard]] std::string base64UrlEncode(std::span<const std::byte> input);

[[nodiscard]] std::optional<std::vector<std::byte>>
base64UrlDecodeBounded(std::string_view input, std::size_t maximumOutputBytes,
                       std::string &diagnostic);

} // namespace replay
