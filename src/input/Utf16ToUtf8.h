#pragma once

#include <string>
#include <string_view>

[[nodiscard]] std::string utf16ToUtf8(std::u16string_view value);
