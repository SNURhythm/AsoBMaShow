#pragma once

#include "../ThreadCompat.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ir {

inline constexpr std::size_t kMaximumIrHttpResponseBytes =
    64U * 1024U * 1024U;

enum class IrHttpMethod { Get, Post };

enum class IrTransportError {
  None,
  Cancelled,
  Offline,
  Dns,
  Connect,
  Tls,
  Timeout,
  ResponseTooLarge,
  Other,
};

struct IrHttpRequest {
  IrHttpMethod method = IrHttpMethod::Get;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::size_t maximumResponseBytes = 1024 * 1024;
  std::chrono::seconds connectTimeout{10};
  std::chrono::seconds totalTimeout{25};
  bool followRedirects = false;
};

struct IrHttpResponse {
  long statusCode = 0;
  std::string body;
  std::optional<std::string> retryAfter;
  IrTransportError transportError = IrTransportError::None;
  std::string diagnostic;
};

class IrHttpClient {
public:
  virtual ~IrHttpClient() = default;
  virtual IrHttpResponse perform(const IrHttpRequest &request,
                                 std::stop_token stopToken) noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IrHttpClient> CreatePlatformIrHttpClient();

namespace http_testing {

struct TransportResponse {
  long statusCode = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::string> bodyChunks;
  IrTransportError transportError = IrTransportError::None;
  std::string diagnostic;
};

using Transport = std::function<TransportResponse(const IrHttpRequest &,
                                                  std::stop_token)>;

[[nodiscard]] IrHttpResponse
PerformWithTransport(const IrHttpRequest &request, std::stop_token stopToken,
                     const Transport &transport) noexcept;

} // namespace http_testing
} // namespace ir
