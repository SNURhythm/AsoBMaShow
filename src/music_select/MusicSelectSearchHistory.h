#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

class MusicSelectSearchHistory final {
public:
  static constexpr std::size_t kDefaultMaximum = 10;

  [[nodiscard]] static bool acceptsText(std::string_view text);
  [[nodiscard]] bool remember(std::string text, bool hasResults,
                              std::size_t maximum = kDefaultMaximum);
  [[nodiscard]] const std::vector<std::string> &entries() const noexcept {
    return entries_;
  }

private:
  std::vector<std::string> entries_;
};
