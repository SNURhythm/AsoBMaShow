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

struct IrProviderSettings {
  bool enabled = false;
  bool autoSubmit = false;
  std::string serverOrigin = std::string(kDefaultTachiServerOrigin);

  bool operator==(const IrProviderSettings &) const = default;
};

[[nodiscard]] std::optional<std::string>
normalizeServerOrigin(std::string_view value) noexcept;

void sanitizeProviderSettings(IrProviderSettings &settings) noexcept;

} // namespace ir
