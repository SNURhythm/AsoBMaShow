#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ir {

inline constexpr std::string_view kTachiProviderId = "tachi";
inline constexpr std::string_view kDefaultTachiServerOrigin =
    "https://boku.tachi.ac";
inline constexpr std::size_t kMaximumServerOriginBytes = 2 * 1024;
inline constexpr std::size_t kMaximumIrProviderIdBytes = 64;

[[nodiscard]] inline bool
isValidProviderId(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaximumIrProviderIdBytes ||
      value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  for (const unsigned char character : value) {
    if (!((character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '_' ||
          character == '-')) {
      return false;
    }
  }
  return true;
}

struct IrProviderSettings {
  bool enabled = false;
  bool autoSubmit = false;
  std::string serverOrigin = std::string(kDefaultTachiServerOrigin);

  bool operator==(const IrProviderSettings &) const = default;
};

[[nodiscard]] std::optional<std::string>
normalizeServerOrigin(std::string_view value) noexcept;

[[nodiscard]] bool
isHttpsServerOrigin(std::string_view value) noexcept;

void sanitizeProviderSettings(IrProviderSettings &settings) noexcept;

} // namespace ir
