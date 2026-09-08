#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace platform_open {

struct RevealAnchor {
  float x = 0.5f;
  float y = 0.5f;
  float width = 0.0f;
  float height = 0.0f;
};

[[nodiscard]] bool desktopOpenSupported() noexcept;
[[nodiscard]] inline bool isWebUrl(std::string_view url) noexcept {
  const auto schemeEnd = url.find("://");
  if (schemeEnd != 4 && schemeEnd != 5) return false;
  const auto scheme = url.substr(0, schemeEnd);
  const std::string_view expected = schemeEnd == 4 ? "http" : "https";
  for (std::size_t index = 0; index < scheme.size(); ++index) {
    const char value = scheme[index];
    const char lower = value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
    if (lower != expected[index]) return false;
  }
  const auto authority = url.substr(schemeEnd + 3);
  if (authority.empty() || authority.find_first_of("/?#") == 0) return false;
  for (const unsigned char value : url) {
    if (value <= 0x20 || value == 0x7f || value == '\\') return false;
  }
  return true;
}
[[nodiscard]] bool revealPathInFileManager(
    const std::filesystem::path &, const RevealAnchor &, std::string &error);
[[nodiscard]] bool openPath(const std::filesystem::path &,
                            std::string &error);
[[nodiscard]] bool openExternalUrl(std::string_view, std::string &error);

} // namespace platform_open
