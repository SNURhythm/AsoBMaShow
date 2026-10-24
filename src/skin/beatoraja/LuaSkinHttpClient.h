#pragma once

#include <cstddef>
#include <chrono>
#include <stop_token>
#include <span>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

struct LuaSkinHttpLimits {
  std::size_t maximumBytes = 1024 * 1024;
  std::stop_token requestStop;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  bool restrictedDestinations = false;
};

struct LuaSkinHttpResponse {
  int responseCode = 0;
  std::string body;
};

struct LuaSkinHttpResult {
  std::optional<LuaSkinHttpResponse> response;
  std::optional<std::string> failure;
};

struct LuaSkinHttpCodeResult {
  std::optional<int> code;
  std::optional<std::string> failure;
};

struct LuaSkinHttpBodyResult {
  std::optional<std::string> body;
  std::optional<std::string> failure;
};

class LuaSkinHttpConnection {
public:
  virtual ~LuaSkinHttpConnection() = default;

  virtual std::optional<std::string> connect() noexcept = 0;
  virtual LuaSkinHttpCodeResult responseCode() noexcept = 0;
  virtual LuaSkinHttpBodyResult readBody() noexcept = 0;
  virtual void disconnect() noexcept = 0;
  virtual void setResponseLimitBytes(std::size_t) noexcept {}
};

struct LuaSkinHttpOpenResult {
  std::unique_ptr<LuaSkinHttpConnection> connection;
  std::optional<std::string> failure;
};

class LuaSkinHttpTransport {
public:
  virtual ~LuaSkinHttpTransport() = default;

  virtual LuaSkinHttpOpenResult open(std::string_view url,
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
  inline static const LuaSkinHttpLimits responseLimits{};
  inline static constexpr std::size_t defaultMaximumBytes = 1024 * 1024;
  inline static constexpr std::size_t maximumBytes = 8 * 1024 * 1024;

  explicit LuaSkinHttpClient(LuaSkinHttpTransport *transport) noexcept;

  [[nodiscard]] LuaSkinHttpOpenResult
  open(std::string_view url, int timeoutMilliseconds,
       std::size_t maxBytes = defaultMaximumBytes) const noexcept;
  [[nodiscard]] LuaSkinHttpResult get(std::string_view url,
                                      int timeoutMilliseconds,
                                      std::size_t maxBytes = defaultMaximumBytes) const noexcept;
  [[nodiscard]] static LuaSkinHttpLinesResult
  readLines(std::string_view body,
            std::size_t maxBytes = defaultMaximumBytes) noexcept;
  [[nodiscard]] static std::optional<std::size_t> checkedLimit(double value) noexcept;
  // Syntax validation is nonblocking; DNS validation belongs on HTTP workers.
  [[nodiscard]] static std::optional<std::string> validateAsyncUrl(std::string_view url);
  [[nodiscard]] static std::optional<std::string> validateAsyncRedirect(
      std::string_view previous, std::string_view next, int redirectCount);
  [[nodiscard]] static bool isPublicAddress(std::span<const unsigned char> bytes) noexcept;
  [[nodiscard]] static int clampTimeout(int timeoutMilliseconds) noexcept;

private:
  LuaSkinHttpTransport *transport_ = nullptr;
};

} // namespace skin
