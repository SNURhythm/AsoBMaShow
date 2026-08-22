#pragma once

#include <cstddef>
#include <memory>
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
  inline static constexpr LuaSkinHttpLimits responseLimits{};

  explicit LuaSkinHttpClient(LuaSkinHttpTransport *transport) noexcept;

  [[nodiscard]] LuaSkinHttpOpenResult
  open(std::string_view url, int timeoutMilliseconds) const noexcept;
  [[nodiscard]] LuaSkinHttpResult get(std::string_view url,
                                      int timeoutMilliseconds) const noexcept;
  [[nodiscard]] static LuaSkinHttpLinesResult
  readLines(std::string_view body) noexcept;
  [[nodiscard]] static int clampTimeout(int timeoutMilliseconds) noexcept;

private:
  LuaSkinHttpTransport *transport_ = nullptr;
};

} // namespace skin
