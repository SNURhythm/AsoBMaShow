#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace skin {

// A copyable compiled-pattern value for the java.util.regex.Pattern surface
// used by SkinFileLuaApiExporter.  Implementations use PCRE2 on CMake-built
// desktop/Android targets and Foundation's ICU engine in the iOS application.
class LuaSkinJavaPattern final {
public:
  static std::optional<LuaSkinJavaPattern> compile(std::string_view pattern);

  [[nodiscard]] std::optional<std::string>
  find(std::string_view subject) const noexcept;

private:
  struct Impl;
  explicit LuaSkinJavaPattern(std::shared_ptr<const Impl>) noexcept;
  std::shared_ptr<const Impl> impl_;
};

} // namespace skin
