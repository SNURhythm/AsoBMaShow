#pragma once
#include "LuaSkinHttpClient.h"

namespace skin {
struct LuaSkinAsyncHttpSubmitResult {
  std::optional<int> id;
  std::optional<std::string> failure;
};
struct LuaSkinAsyncHttpResult {
  std::string status = "pending";
  int statusCode = 0;
  std::string body;
  std::string error;
};

// A skin owns this facade. Connections and capacity outlive it while cancelled
// workers unwind; neither workers nor the shared scheduler ever call Lua.
class LuaSkinAsyncHttp final {
public:
  explicit LuaSkinAsyncHttp(LuaSkinHttpTransport *transport);
  ~LuaSkinAsyncHttp();
  LuaSkinAsyncHttp(const LuaSkinAsyncHttp &) = delete;
  LuaSkinAsyncHttp &operator=(const LuaSkinAsyncHttp &) = delete;
  [[nodiscard]] LuaSkinAsyncHttpSubmitResult submit(
      std::string_view url, int timeoutMilliseconds = 1000,
      std::size_t maxBytes = LuaSkinHttpClient::defaultMaximumBytes) noexcept;
  [[nodiscard]] LuaSkinAsyncHttpResult poll(int id) noexcept;
  bool cancel(int id) noexcept;
  void dispose() noexcept;
  [[nodiscard]] std::size_t reservedBytes() const noexcept;
private:
  struct State;
  struct Job;
  struct Workers;
  static Workers &workers();
  LuaSkinHttpTransport *transport_;
  std::shared_ptr<State> state_;
};
} // namespace skin
