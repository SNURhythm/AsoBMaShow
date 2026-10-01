#pragma once

#include <string_view>
#include <vector>

namespace asobmashow::text {

// Strict Unicode scalar validation; embedded NUL and noncharacters are valid.
bool validUtf8(std::string_view value) noexcept;

// Reuses output storage. Invalid input clears output, exposing no partial text.
bool decodeUtf8(std::string_view value, std::vector<char32_t> &output);

} // namespace asobmashow::text
