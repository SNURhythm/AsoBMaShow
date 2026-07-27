#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace replay {

[[nodiscard]] std::optional<std::vector<std::byte>>
gzipCompress(std::span<const std::byte> input, std::string &diagnostic);

[[nodiscard]] std::optional<std::vector<std::byte>>
gzipDecompressBounded(std::span<const std::byte> input,
                      std::size_t maximumOutputBytes, std::string &diagnostic);

} // namespace replay
