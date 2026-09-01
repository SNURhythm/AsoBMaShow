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
[[nodiscard]] bool revealPathInFileManager(
    const std::filesystem::path &, const RevealAnchor &, std::string &error);
[[nodiscard]] bool openPath(const std::filesystem::path &,
                            std::string &error);
[[nodiscard]] bool openExternalUrl(std::string_view, std::string &error);

} // namespace platform_open
