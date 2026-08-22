#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

struct LuaSkinHttpLimits {
  std::size_t maximumLines = 1024;
  std::size_t maximumCharacters = 65536;
};

struct LuaSkinHttpResponse {
  int responseCode = 0;
  std::string body;
};

struct LuaSkinHttpResult {
  std::optional<LuaSkinHttpResponse> response;
  std::optional<std::string> failure;
};

class LuaSkinHttpTransport {
public:
  virtual ~LuaSkinHttpTransport() = default;

  virtual LuaSkinHttpResult get(std::string_view url,
                                int timeoutMilliseconds,
                                LuaSkinHttpLimits limits) = 0;
};

struct LuaSkinHttpLinesResult {
  std::vector<std::string> lines;
  std::optional<std::string> failure;
};

class LuaSkinHttpClient final {
public:
  inline static constexpr int defaultTimeoutMilliseconds = 1000;
  inline static constexpr int maximumTimeoutMilliseconds = 5000;
  inline static constexpr LuaSkinHttpLimits responseLimits{};

  explicit LuaSkinHttpClient(LuaSkinHttpTransport *transport) noexcept;

  [[nodiscard]] LuaSkinHttpResult get(std::string_view url,
                                      int timeoutMilliseconds) const noexcept;
  [[nodiscard]] static LuaSkinHttpLinesResult
  readLines(std::string_view body) noexcept;
  [[nodiscard]] static int clampTimeout(int timeoutMilliseconds) noexcept;

private:
  LuaSkinHttpTransport *transport_ = nullptr;
};

} // namespace skin
