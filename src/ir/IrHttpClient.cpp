#include "IrHttpClient.h"

#include "../targets.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <string_view>

#if TARGET_OS_IOS || TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
#include "IrHttpClientIOS.h"
#else
#include "../CurlRAII.h"
#endif

namespace ir {
namespace {

constexpr std::size_t kMaximumUrlBytes = 8 * 1024;
constexpr std::size_t kMaximumRequestBodyBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumResponseBodyBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaximumHeaderCount = 64;
constexpr std::size_t kMaximumHeaderNameBytes = 128;
constexpr std::size_t kMaximumHeaderValueBytes = 8 * 1024;
constexpr std::size_t kMaximumSafeResponseHeaderBytes = 2 * 1024;
constexpr std::size_t kMaximumDiagnosticBytes = 512;

bool asciiCaseEqual(std::string_view first, std::string_view second) {
  return first.size() == second.size() &&
         std::ranges::equal(first, second, [](unsigned char left,
                                             unsigned char right) {
           return std::tolower(left) == std::tolower(right);
         });
}

bool validHeaderName(std::string_view name) {
  if (name.empty() || name.size() > kMaximumHeaderNameBytes) {
    return false;
  }
  constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
  return std::ranges::all_of(name, [&](unsigned char character) {
    return std::isalnum(character) != 0 ||
           punctuation.find(static_cast<char>(character)) !=
               std::string_view::npos;
  });
}

bool validHeaderValue(std::string_view value, std::size_t maximum) {
  return value.size() <= maximum &&
         std::ranges::all_of(value, [](unsigned char character) {
           return character == '\t' || (character >= 0x20U && character < 0x7fU);
         });
}

std::string trimHorizontalWhitespace(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

std::optional<std::string> safeHeader(
    const std::vector<std::pair<std::string, std::string>> &headers,
    std::string_view expectedName) {
  std::optional<std::string> found;
  for (const auto &[name, value] : headers) {
    if (!asciiCaseEqual(name, expectedName)) {
      continue;
    }
    if (found || !validHeaderValue(value, kMaximumSafeResponseHeaderBytes)) {
      return std::nullopt;
    }
    std::string trimmed = trimHorizontalWhitespace(value);
    if (trimmed.empty()) {
      return std::nullopt;
    }
    found = std::move(trimmed);
  }
  return found;
}

std::string boundedDiagnostic(std::string_view value) {
  std::string result(value.substr(0, kMaximumDiagnosticBytes));
  for (char &character : result) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if ((byte < 0x20U && character != '\t' && character != '\n') ||
        byte == 0x7fU) {
      character = ' ';
    }
  }
  return result;
}

bool validHttpUrl(std::string_view url) {
  if (url.empty() || url.size() > kMaximumUrlBytes ||
      !std::ranges::all_of(url, [](unsigned char character) {
        return character > 0x20U && character < 0x7fU;
      })) {
    return false;
  }
  std::size_t schemeBytes = 0;
  if (url.starts_with("https://")) {
    schemeBytes = 8;
  } else if (url.starts_with("http://")) {
    schemeBytes = 7;
  } else {
    return false;
  }
  const std::size_t authorityEnd = url.find_first_of("/?#", schemeBytes);
  const std::string_view authority = url.substr(
      schemeBytes, authorityEnd == std::string_view::npos
                       ? std::string_view::npos
                       : authorityEnd - schemeBytes);
  return !authority.empty() && authority.find('@') == std::string_view::npos;
}

bool validateRequest(const IrHttpRequest &request, std::string &diagnostic) {
  if (request.method != IrHttpMethod::Get &&
      request.method != IrHttpMethod::Post) {
    diagnostic = "IR HTTP method is invalid";
    return false;
  }
  if (!validHttpUrl(request.url)) {
    diagnostic = "IR HTTP URL is invalid";
    return false;
  }
  if (request.headers.size() > kMaximumHeaderCount) {
    diagnostic = "IR HTTP request has too many headers";
    return false;
  }
  bool authenticated = false;
  for (const auto &[name, value] : request.headers) {
    if (!validHeaderName(name) ||
        !validHeaderValue(value, kMaximumHeaderValueBytes)) {
      diagnostic = "IR HTTP request header is invalid";
      return false;
    }
    authenticated = authenticated || asciiCaseEqual(name, "Authorization");
  }
  if (authenticated && request.followRedirects) {
    diagnostic = "authenticated IR HTTP redirects are forbidden";
    return false;
  }
  if (request.method == IrHttpMethod::Get && !request.body.empty()) {
    diagnostic = "IR HTTP GET request cannot contain a body";
    return false;
  }
  if (request.body.size() > kMaximumRequestBodyBytes ||
      request.maximumResponseBytes == 0 ||
      request.maximumResponseBytes > kMaximumResponseBodyBytes) {
    diagnostic = "IR HTTP request or response size limit is invalid";
    return false;
  }
  const auto connectSeconds = request.connectTimeout.count();
  const auto totalSeconds = request.totalTimeout.count();
  if (connectSeconds <= 0 || totalSeconds <= 0 || connectSeconds > 300 ||
      totalSeconds > 300 || connectSeconds > totalSeconds) {
    diagnostic = "IR HTTP timeout is invalid";
    return false;
  }
  diagnostic.clear();
  return true;
}

IrHttpResponse invalidResponse(std::string diagnostic) {
  return {.transportError = IrTransportError::Other,
          .diagnostic = std::move(diagnostic)};
}

IrHttpResponse cancelledResponse() {
  return {.transportError = IrTransportError::Cancelled,
          .diagnostic = "IR HTTP request was cancelled"};
}

#if !(TARGET_OS_IOS || TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)

struct CurlRequestContext {
  std::string body;
  std::size_t maximumBytes = 0;
  std::stop_token stopToken;
  bool responseTooLarge = false;
  bool retryAfterSeen = false;
  bool retryAfterInvalid = false;
  std::optional<std::string> retryAfter;
};

std::size_t curlWrite(char *data, std::size_t size, std::size_t count,
                      void *opaque) {
  auto *context = static_cast<CurlRequestContext *>(opaque);
  if (context == nullptr || context->stopToken.stop_requested() ||
      (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)) {
    return 0;
  }
  const std::size_t bytes = size * count;
  if (bytes > context->maximumBytes - context->body.size()) {
    context->responseTooLarge = true;
    return 0;
  }
  context->body.append(data, bytes);
  return bytes;
}

std::size_t curlHeader(char *data, std::size_t size, std::size_t count,
                       void *opaque) {
  auto *context = static_cast<CurlRequestContext *>(opaque);
  if (context == nullptr ||
      (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)) {
    return 0;
  }
  const std::size_t bytes = size * count;
  std::string_view line(data, bytes);
  const std::size_t colon = line.find(':');
  if (colon == std::string_view::npos ||
      !asciiCaseEqual(line.substr(0, colon), "Retry-After")) {
    return bytes;
  }
  if (context->retryAfterSeen) {
    context->retryAfterInvalid = true;
    context->retryAfter.reset();
    return bytes;
  }
  context->retryAfterSeen = true;
  std::string_view value = line.substr(colon + 1);
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
    value.remove_suffix(1);
  }
  if (!validHeaderValue(value, kMaximumSafeResponseHeaderBytes)) {
    context->retryAfterInvalid = true;
    return bytes;
  }
  std::string trimmed = trimHorizontalWhitespace(value);
  if (trimmed.empty()) {
    context->retryAfterInvalid = true;
  } else {
    context->retryAfter = std::move(trimmed);
  }
  return bytes;
}

int curlProgress(void *opaque, curl_off_t, curl_off_t, curl_off_t,
                 curl_off_t) {
  const auto *context = static_cast<const CurlRequestContext *>(opaque);
  return context != nullptr && context->stopToken.stop_requested() ? 1 : 0;
}

IrTransportError mapCurlError(CURLcode error,
                              const CurlRequestContext &context) {
  if (context.stopToken.stop_requested()) {
    return IrTransportError::Cancelled;
  }
  if (context.responseTooLarge) {
    return IrTransportError::ResponseTooLarge;
  }
  switch (error) {
  case CURLE_OK:
    return IrTransportError::None;
  case CURLE_ABORTED_BY_CALLBACK:
    return IrTransportError::Cancelled;
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_RESOLVE_PROXY:
    return IrTransportError::Dns;
  case CURLE_COULDNT_CONNECT:
    return IrTransportError::Connect;
  case CURLE_SSL_CONNECT_ERROR:
  case CURLE_PEER_FAILED_VERIFICATION:
  case CURLE_SSL_CERTPROBLEM:
  case CURLE_SSL_CIPHER:
  case CURLE_SSL_CACERT_BADFILE:
    return IrTransportError::Tls;
  case CURLE_OPERATION_TIMEDOUT:
    return IrTransportError::Timeout;
  case CURLE_INTERFACE_FAILED:
    return IrTransportError::Offline;
  default:
    return IrTransportError::Other;
  }
}

class CurlIrHttpClient final : public IrHttpClient {
public:
  IrHttpResponse perform(const IrHttpRequest &request,
                         std::stop_token stopToken) noexcept override {
    try {
      std::string diagnostic;
      if (!validateRequest(request, diagnostic)) {
        return invalidResponse(std::move(diagnostic));
      }
      if (stopToken.stop_requested()) {
        return cancelledResponse();
      }

      static std::once_flag initializeOnce;
      static CURLcode initialization = CURLE_FAILED_INIT;
      std::call_once(initializeOnce, [] {
        initialization = curl_global_init(CURL_GLOBAL_DEFAULT);
      });
      if (initialization != CURLE_OK) {
        return invalidResponse("IR HTTP transport initialization failed");
      }
      CurlEasyHandle curl(curl_easy_init());
      if (!curl) {
        return invalidResponse("IR HTTP transport allocation failed");
      }

      UniqueResource<curl_slist, curl_slist_free_all> headerList(nullptr);
      for (const auto &[name, value] : request.headers) {
        const std::string encoded = name + ": " + value;
        curl_slist *next = curl_slist_append(headerList.get(), encoded.c_str());
        if (next == nullptr) {
          return invalidResponse("IR HTTP header allocation failed");
        }
        (void)headerList.release();
        headerList.reset(next);
      }

      CurlRequestContext context{.maximumBytes = request.maximumResponseBytes,
                                 .stopToken = stopToken};
      char curlError[CURL_ERROR_SIZE] = {};
      bool optionsSet =
          curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str()) ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 0L) == CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https") ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT,
                           static_cast<long>(request.connectTimeout.count())) ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT,
                           static_cast<long>(request.totalTimeout.count())) ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "AsoBMaShow") ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curlWrite) ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &context) == CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, curlHeader) ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &context) == CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, curlProgress) ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &context) ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError) ==
              CURLE_OK &&
          curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headerList.get()) ==
              CURLE_OK;
      if (request.method == IrHttpMethod::Post) {
        optionsSet = optionsSet &&
                     curl_easy_setopt(curl.get(), CURLOPT_POST, 1L) == CURLE_OK &&
                     curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS,
                                      request.body.data()) == CURLE_OK &&
                     curl_easy_setopt(
                         curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(request.body.size())) ==
                         CURLE_OK;
      } else {
        optionsSet = optionsSet &&
                     curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L) ==
                         CURLE_OK;
      }
      ConfigureCurlTrustStore(curl.get());
      if (!optionsSet) {
        return invalidResponse("IR HTTP transport configuration failed");
      }

      const CURLcode result = curl_easy_perform(curl.get());
      const IrTransportError mapped = mapCurlError(result, context);
      if (mapped != IrTransportError::None) {
        return {.transportError = mapped,
                .diagnostic = mapped == IrTransportError::Cancelled
                                  ? "IR HTTP request was cancelled"
                              : mapped == IrTransportError::ResponseTooLarge
                                  ? "IR HTTP response exceeded its size limit"
                                  : boundedDiagnostic(curl_easy_strerror(result))};
      }
      long statusCode = 0;
      if (curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &statusCode) !=
              CURLE_OK ||
          statusCode < 100 || statusCode > 599) {
        return invalidResponse("IR HTTP response status is invalid");
      }
      return {.statusCode = statusCode,
              .body = std::move(context.body),
              .retryAfter = context.retryAfterInvalid
                                ? std::nullopt
                                : std::move(context.retryAfter)};
    } catch (...) {
      return invalidResponse("IR HTTP transport failed unexpectedly");
    }
  }
};

#else

class IOSIrHttpClient final : public IrHttpClient {
public:
  IrHttpResponse perform(const IrHttpRequest &request,
                         std::stop_token stopToken) noexcept override {
    std::string diagnostic;
    if (!validateRequest(request, diagnostic)) {
      return invalidResponse(std::move(diagnostic));
    }
    if (stopToken.stop_requested()) {
      return cancelledResponse();
    }
    return PerformIrHttpRequestIOS(request, stopToken);
  }
};

#endif

} // namespace

namespace http_testing {

IrHttpResponse PerformWithTransport(const IrHttpRequest &request,
                                    std::stop_token stopToken,
                                    const Transport &transport) noexcept {
  try {
    std::string diagnostic;
    if (!validateRequest(request, diagnostic)) {
      return invalidResponse(std::move(diagnostic));
    }
    if (stopToken.stop_requested()) {
      return cancelledResponse();
    }
    if (!transport) {
      return invalidResponse("IR HTTP test transport is unavailable");
    }
    TransportResponse raw = transport(request, stopToken);
    if (stopToken.stop_requested()) {
      return cancelledResponse();
    }
    if (raw.transportError != IrTransportError::None) {
      return {.transportError = raw.transportError,
              .diagnostic = boundedDiagnostic(raw.diagnostic)};
    }
    if (raw.statusCode < 100 || raw.statusCode > 599) {
      return invalidResponse("IR HTTP response status is invalid");
    }
    std::string body;
    body.reserve(std::min(request.maximumResponseBytes, std::size_t{4096}));
    for (const std::string &chunk : raw.bodyChunks) {
      if (chunk.size() > request.maximumResponseBytes - body.size()) {
        return {.transportError = IrTransportError::ResponseTooLarge,
                .diagnostic =
                    "IR HTTP response exceeded its size limit"};
      }
      body += chunk;
    }
    return {.statusCode = raw.statusCode,
            .body = std::move(body),
            .retryAfter = safeHeader(raw.headers, "Retry-After"),
            .diagnostic = boundedDiagnostic(raw.diagnostic)};
  } catch (...) {
    return invalidResponse("IR HTTP test transport failed unexpectedly");
  }
}

} // namespace http_testing

std::unique_ptr<IrHttpClient> CreatePlatformIrHttpClient() {
#if TARGET_OS_IOS || TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
  return std::make_unique<IOSIrHttpClient>();
#else
  return std::make_unique<CurlIrHttpClient>();
#endif
}

} // namespace ir
