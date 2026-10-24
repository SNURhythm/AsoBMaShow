#include "LuaSkinCurlHttpTransport.h"
#if defined(ASOBMASHOW_TEST_CURL_HTTP_TRANSPORT)
#include "LuaSkinCurlHttpTransportTest.h"
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(__APPLE__) || !TARGET_OS_IPHONE

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "../../CurlRAII.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace skin {
namespace {

std::once_flag curlInitialization;
CURLcode curlInitializationResult = CURLE_FAILED_INIT;

#if defined(ASOBMASHOW_TEST_CURL_HTTP_TRANSPORT)
std::mutex testDnsMutex;
std::string testDnsServers;

std::string snapshotTestDnsServers() {
  std::lock_guard lock(testDnsMutex);
  return testDnsServers;
}
#endif

bool initializeCurl() noexcept {
  std::call_once(curlInitialization, [] {
    curlInitializationResult = curl_global_init(CURL_GLOBAL_DEFAULT);
  });
  return curlInitializationResult == CURLE_OK;
}

std::string curlFailure(
    CURLcode code, const std::array<char, CURL_ERROR_SIZE> &error) {
  if (error.front() != '\0') {
    return error.data();
  }
  const char *message = curl_easy_strerror(code);
  return message != nullptr ? message : "libcurl request failed";
}

class CurlLuaSkinHttpConnection final : public LuaSkinHttpConnection {
public:
  CurlLuaSkinHttpConnection(std::string url, int timeoutMilliseconds,
                            LuaSkinHttpLimits limits, std::stop_token stop)
      : url_(std::move(url)), timeoutMilliseconds_(timeoutMilliseconds),
        limits_(limits), stop_(stop) {}

  ~CurlLuaSkinHttpConnection() override { disconnect(); }

  std::optional<std::string> connect() noexcept override {
    if (connected_) {
      return std::nullopt;
    }
    if (disconnected_) {
      return "HTTP connection is disconnected";
    }
    if (auto failure = stopped()) return failure;
    if (!initialize()) {
      return initializationFailure_;
    }
    if (limits_.restrictedDestinations) {
      // The multi interface resolves asynchronously and is polled with the total
      // deadline. Actual socket addresses are checked before socket creation.
      connected_ = true;
      return std::nullopt;
    }
    if (curl_easy_setopt(curl_.get(), CURLOPT_CONNECT_ONLY, 1L) != CURLE_OK) {
      return "libcurl connect stage could not be configured";
    }
    error_.fill('\0');
    const CURLcode result = curl_easy_perform(curl_.get());
    if (auto failure = stopped()) return failure;
    if (result != CURLE_OK) {
      return curlFailure(result, error_);
    }
    connected_ = true;
    return std::nullopt;
  }

  LuaSkinHttpCodeResult responseCode() noexcept override {
    if (auto failure = connect()) {
      return {.failure = std::move(failure)};
    }
    if (auto failure = startResponse()) {
      return {.failure = std::move(failure)};
    }
    if (!responseHeadersReady_) {
      if (auto failure = pump(TransferPhase::Response)) {
        return {.failure = std::move(failure)};
      }
    }
    long response = 0;
    const CURLcode info =
        curl_easy_getinfo(curl_.get(), CURLINFO_RESPONSE_CODE, &response);
    if (info != CURLE_OK || response <= 0 ||
        response > std::numeric_limits<int>::max()) {
      return {.failure = transferCompleted_ && transferResult_ != CURLE_OK
                             ? curlFailure(transferResult_, error_)
                             : "HTTP response code is unavailable"};
    }
    responseCode_ = static_cast<int>(response);
    return {.code = responseCode_};
  }

  LuaSkinHttpBodyResult readBody() noexcept override {
    if (auto failure = connect()) {
      return {.failure = std::move(failure)};
    }
    const LuaSkinHttpCodeResult code = responseCode();
    if (code.failure) {
      return {.failure = code.failure};
    }
    if (body_.size() > limits_.maximumBytes) return {.failure = "response is too large"};
    readRequested_ = true;
    if (responsePaused_) {
      const CURLcode resumed = curl_easy_pause(curl_.get(), CURLPAUSE_CONT);
      responsePaused_ = false;
      collectCompletion();
      if (resumed != CURLE_OK && !transferCompleted_ && !intentionalStop_) {
        return {.failure = curlFailure(resumed, error_)};
      }
    }
    if (!transferCompleted_) {
      if (auto failure = pump(TransferPhase::Read)) {
        readFailure_ = *failure;
      }
    }
    finishRead();
    if (readFailure_) {
      return {.failure = *readFailure_};
    }
    return {.body = body_};
  }

  void setResponseLimitBytes(std::size_t bytes) noexcept override {
    if (bytes >= 1 && bytes <= LuaSkinHttpClient::maximumBytes) limits_.maximumBytes = bytes;
  }

  void disconnect() noexcept override {
    if (disconnected_) {
      return;
    }
    disconnected_ = true;
    if (multi_ != nullptr) {
      if (multiContainsEasy_ && curl_) {
        (void)curl_multi_remove_handle(multi_, curl_.get());
      }
      curl_multi_cleanup(multi_);
      multi_ = nullptr;
      multiContainsEasy_ = false;
    }
    curl_.reset();
  }

  const std::string &redirectLocation() const noexcept { return redirectLocation_; }

private:
  std::optional<std::string> stopped() const {
    if (limits_.requestStop.stop_requested() || stop_.stop_requested()) return "HTTP request was cancelled";
    if (std::chrono::steady_clock::now() >= limits_.deadline) return "HTTP request timed out";
    return std::nullopt;
  }

  enum class TransferPhase { Response, Read };

  bool initialize() noexcept {
    if (curl_) {
      return true;
    }
    if (!initializeCurl()) {
      initializationFailure_ = "libcurl initialization failed";
      return false;
    }
    const auto *version = curl_version_info(CURLVERSION_NOW);
    // ASYNCHDNS also describes the threaded resolver, whose cleanup can join
    // blocked system DNS calls after our deadline. Require c-ares so removing
    // a transfer releases the worker without waiting for a resolver thread.
    if (limits_.restrictedDestinations &&
        (!version || !(version->features & CURL_VERSION_ASYNCHDNS) ||
         !version->ares || version->ares[0] == '\0')) {
      initializationFailure_ = "Cancellable HTTP DNS resolution requires libcurl with c-ares";
      return false;
    }
    curl_.reset(curl_easy_init());
    if (!curl_) {
      initializationFailure_ = "libcurl connection allocation failed";
      return false;
    }
    error_.fill('\0');
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        limits_.deadline - std::chrono::steady_clock::now()).count();
    const long timeout = static_cast<long>(std::max<std::int64_t>(1,
        std::min<std::int64_t>(timeoutMilliseconds_, remaining)));
    const bool configured =
        curl_easy_setopt(curl_.get(), CURLOPT_URL, url_.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_HTTPGET, 1L) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_HTTP_VERSION,
                         CURL_HTTP_VERSION_1_1) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_CONNECTTIMEOUT_MS,
                         timeout) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_TIMEOUT_MS,
                         timeout) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_FOLLOWLOCATION, limits_.restrictedDestinations ? 0L : 1L) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_MAXREDIRS, 8L) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_PROTOCOLS_STR, "http,https") ==
            CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_REDIR_PROTOCOLS_STR,
                         "http,https") == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_USERAGENT, "AsoBMaShow") ==
            CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_ERRORBUFFER, error_.data()) ==
            CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_XFERINFOFUNCTION,
                         &CurlLuaSkinHttpConnection::progress) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_XFERINFODATA, this) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_NOPROGRESS, 0L) == CURLE_OK;
    if (!configured) {
      initializationFailure_ = "libcurl connection could not be configured";
      curl_.reset();
      return false;
    }
#if defined(ASOBMASHOW_TEST_CURL_HTTP_TRANSPORT)
    if (!testDnsServers_.empty() &&
        curl_easy_setopt(curl_.get(), CURLOPT_DNS_SERVERS, testDnsServers_.c_str()) != CURLE_OK) {
      initializationFailure_ = "Test DNS server could not be configured";
      curl_.reset();
      return false;
    }
#endif
    if (limits_.restrictedDestinations &&
        (curl_easy_setopt(curl_.get(), CURLOPT_PROXY, "") != CURLE_OK ||
         curl_easy_setopt(curl_.get(), CURLOPT_OPENSOCKETFUNCTION, &CurlLuaSkinHttpConnection::openSocket) != CURLE_OK ||
         curl_easy_setopt(curl_.get(), CURLOPT_OPENSOCKETDATA, this) != CURLE_OK)) {
      initializationFailure_ = "HTTP destination policy could not be configured";
      curl_.reset();
      return false;
    }
    if (!ConfigureCurlTrustStore(curl_.get())) {
      initializationFailure_ = "HTTP certificate trust store could not be configured";
      curl_.reset();
      return false;
    }
    return true;
  }

  std::optional<std::string> startResponse() noexcept {
    if (responseStarted_) {
      return responseFailure_;
    }
    responseStarted_ = true;
    body_.clear();
    readFailure_.reset();
    responseFailure_.reset();
    intentionalStop_ = false;
    tooLarge_ = false;
    allocationFailed_ = false;
    error_.fill('\0');
    const bool configured =
        curl_easy_setopt(curl_.get(), CURLOPT_CONNECT_ONLY, 0L) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_WRITEFUNCTION,
                         &CurlLuaSkinHttpConnection::write) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_WRITEDATA, this) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_HEADERFUNCTION,
                         &CurlLuaSkinHttpConnection::header) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_HEADERDATA, this) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L) ==
            CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_TIMEOUT_MS, 0L) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_FAILONERROR, limits_.restrictedDestinations ? 0L : 1L) == CURLE_OK;
    if (!configured) {
      responseFailure_ = "libcurl response stage could not be configured";
      return responseFailure_;
    }

    multi_ = curl_multi_init();
    if (multi_ == nullptr) {
      responseFailure_ = "libcurl multi connection allocation failed";
      return responseFailure_;
    }
    const CURLMcode added = curl_multi_add_handle(multi_, curl_.get());
    if (added != CURLM_OK) {
      responseFailure_ = curl_multi_strerror(added);
      return responseFailure_;
    }
    multiContainsEasy_ = true;
    return std::nullopt;
  }

  std::optional<std::string> pump(TransferPhase phase) noexcept {
    const auto deadline = std::min(limits_.deadline, std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMilliseconds_));
    while (true) {
      if (auto failure = stopped()) return failure;
      int running = 0;
      const CURLMcode performed = curl_multi_perform(multi_, &running);
      if (performed != CURLM_OK) {
        return curl_multi_strerror(performed);
      }
      collectCompletion();
      if (destinationDenied_) return "Non-public HTTP destination denied";
      if (phase == TransferPhase::Response && responseHeadersReady_) {
        return std::nullopt;
      }
      if (transferCompleted_) {
        if (transferResult_ != CURLE_OK && !intentionalStop_) {
          return curlFailure(transferResult_, error_);
        }
        return std::nullopt;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return "HTTP request timed out";
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int waitMilliseconds = static_cast<int>(
          std::min<std::int64_t>(
              10, std::max<std::int64_t>(1, remaining.count())));
      int ready = 0;
      const CURLMcode waited =
          curl_multi_poll(multi_, nullptr, 0, waitMilliseconds, &ready);
      if (waited != CURLM_OK) {
        return curl_multi_strerror(waited);
      }
      (void)running;
      (void)ready;
    }
  }

  void collectCompletion() noexcept {
    int queued = 0;
    while (CURLMsg *message = curl_multi_info_read(multi_, &queued)) {
      if (message->msg == CURLMSG_DONE && message->easy_handle == curl_.get()) {
        transferCompleted_ = true;
        transferResult_ = message->data.result;
      }
    }
  }

  void finishRead() noexcept {
    if (readFinished_) {
      return;
    }
    readFinished_ = true;
    if (allocationFailed_) {
      readFailure_ = "HTTP response allocation failed";
    } else if (tooLarge_) {
      readFailure_ = "response is too large";
    } else if (limits_.requestStop.stop_requested() || stop_.stop_requested() ||
               transferResult_ == CURLE_ABORTED_BY_CALLBACK) {
      readFailure_ = "HTTP request was cancelled";
    } else if (std::chrono::steady_clock::now() >= limits_.deadline) {
      readFailure_ = "HTTP request timed out";
    } else if (transferCompleted_ && transferResult_ != CURLE_OK &&
               !intentionalStop_) {
      readFailure_ = curlFailure(transferResult_, error_);
    }
  }

  static bool isBlankHeader(std::string_view line) noexcept {
    return line == "\r\n" || line == "\n";
  }

  static bool startsWithIgnoringCase(std::string_view value,
                                     std::string_view prefix) noexcept {
    if (value.size() < prefix.size()) {
      return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      const unsigned char left = static_cast<unsigned char>(value[index]);
      const unsigned char right = static_cast<unsigned char>(prefix[index]);
      const unsigned char foldedLeft =
          left >= 'A' && left <= 'Z' ? left - 'A' + 'a' : left;
      const unsigned char foldedRight =
          right >= 'A' && right <= 'Z' ? right - 'A' + 'a' : right;
      if (foldedLeft != foldedRight) {
        return false;
      }
    }
    return true;
  }

  static std::size_t header(char *data, std::size_t size,
                            std::size_t count, void *opaque) noexcept {
    auto *self = static_cast<CurlLuaSkinHttpConnection *>(opaque);
    if (self == nullptr || data == nullptr || size == 0 ||
        count > std::numeric_limits<std::size_t>::max() / size) {
      return 0;
    }
    const std::size_t bytes = size * count;
    const std::string_view line(data, bytes);
    if (line.starts_with("HTTP/")) {
      self->currentResponseStatus_ = 0;
      self->currentResponseHasLocation_ = false;
      self->redirectLocation_.clear();
      const std::size_t space = line.find(' ');
      if (space != std::string_view::npos && space + 3 < line.size() &&
          line[space + 1] >= '0' && line[space + 1] <= '9' &&
          line[space + 2] >= '0' && line[space + 2] <= '9' &&
          line[space + 3] >= '0' && line[space + 3] <= '9') {
        self->currentResponseStatus_ =
            (line[space + 1] - '0') * 100 + (line[space + 2] - '0') * 10 +
            (line[space + 3] - '0');
      }
    } else if (startsWithIgnoringCase(line, "Location:")) {
      self->currentResponseHasLocation_ = true;
      auto location = line.substr(9);
      while (!location.empty() && (location.front() == ' ' || location.front() == '\t')) location.remove_prefix(1);
      while (!location.empty() && (location.back() == '\r' || location.back() == '\n' || location.back() == ' ')) location.remove_suffix(1);
      try { self->redirectLocation_ = location; } catch (...) { return 0; }
    } else if (isBlankHeader(line)) {
      const bool informational = self->currentResponseStatus_ >= 100 &&
                                 self->currentResponseStatus_ < 200;
      const bool redirect = self->currentResponseStatus_ >= 300 &&
                            self->currentResponseStatus_ < 400 &&
                            self->currentResponseHasLocation_;
      if (!informational && (!redirect || self->limits_.restrictedDestinations)) {
        if (curl_easy_pause(self->curl_.get(), CURLPAUSE_RECV) != CURLE_OK) {
          return 0;
        }
        self->responseHeadersReady_ = true;
        self->responsePaused_ = true;
      }
    }
    return bytes;
  }

  std::size_t consume(const char *bytes, std::size_t size) noexcept {
    if (size > limits_.maximumBytes - body_.size()) {
      tooLarge_ = true;
      intentionalStop_ = true;
      return 0;
    }
    try { body_.append(bytes, size); }
    catch (...) { allocationFailed_ = true; intentionalStop_ = true; return 0; }
    return size;
  }

  static std::size_t write(char *data, std::size_t size,
                           std::size_t count, void *opaque) noexcept {
    auto *self = static_cast<CurlLuaSkinHttpConnection *>(opaque);
    if (self == nullptr || data == nullptr || size == 0 ||
        count > std::numeric_limits<std::size_t>::max() / size) {
      return 0;
    }
    if (self->responseHeadersReady_ && !self->readRequested_) {
      self->responsePaused_ = true;
      return CURL_WRITEFUNC_PAUSE;
    }
    return self->consume(data, size * count);
  }

  static int progress(void *opaque, curl_off_t, curl_off_t, curl_off_t,
                      curl_off_t) noexcept {
    const auto *self = static_cast<CurlLuaSkinHttpConnection *>(opaque);
    return self != nullptr && (self->stop_.stop_requested() || self->limits_.requestStop.stop_requested() ||
        std::chrono::steady_clock::now() >= self->limits_.deadline) ? 1 : 0;
  }

  static curl_socket_t openSocket(void *opaque, curlsocktype purpose, struct curl_sockaddr *address) noexcept {
    if (purpose != CURLSOCKTYPE_IPCXN ||
        static_cast<CurlLuaSkinHttpConnection *>(opaque)->destinationDenied_) return CURL_SOCKET_BAD;
    std::span<const unsigned char> bytes;
    if (address->family == AF_INET) {
      auto *ipv4 = reinterpret_cast<const sockaddr_in *>(&address->addr);
      bytes = {reinterpret_cast<const unsigned char *>(&ipv4->sin_addr), 4};
    } else if (address->family == AF_INET6) {
      auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(&address->addr);
      bytes = {reinterpret_cast<const unsigned char *>(&ipv6->sin6_addr), 16};
    }
    if (!LuaSkinHttpClient::isPublicAddress(bytes)) {
      static_cast<CurlLuaSkinHttpConnection *>(opaque)->destinationDenied_ = true;
      return CURL_SOCKET_BAD;
    }
    return ::socket(address->family, address->socktype, address->protocol);
  }

  std::string redirectLocation_;
  std::string url_;
  int timeoutMilliseconds_ = 1000;
  LuaSkinHttpLimits limits_;
  std::stop_token stop_;
#if defined(ASOBMASHOW_TEST_CURL_HTTP_TRANSPORT)
  const std::string testDnsServers_ = snapshotTestDnsServers();
#endif
  CurlEasyHandle curl_{nullptr};
  CURLM *multi_ = nullptr;
  std::array<char, CURL_ERROR_SIZE> error_{};
  std::string initializationFailure_;
  std::string body_;
  std::optional<std::string> responseFailure_;
  std::optional<std::string> readFailure_;
  int responseCode_ = 0;
  int currentResponseStatus_ = 0;
  bool connected_ = false;
  bool responseStarted_ = false;
  bool responseHeadersReady_ = false;
  bool responsePaused_ = false;
  bool currentResponseHasLocation_ = false;
  bool transferCompleted_ = false;
  bool readFinished_ = false;
  bool readRequested_ = false;
  bool multiContainsEasy_ = false;
  bool disconnected_ = false;
  bool intentionalStop_ = false;
  bool tooLarge_ = false;
  bool allocationFailed_ = false;
  bool destinationDenied_ = false;
  CURLcode transferResult_ = CURLE_OK;
};

// Async requests follow redirects explicitly so every destination receives the
// same URL, DNS, deadline and scheme checks before opening a new connection.
class RestrictedCurlHttpConnection final : public LuaSkinHttpConnection {
public:
  RestrictedCurlHttpConnection(std::string url, int timeout, LuaSkinHttpLimits limits, std::stop_token stop)
      : url_(std::move(url)), timeout_(timeout), limits_(limits), stop_(stop) {}
  std::optional<std::string> connect() noexcept override {
    if (disconnected_) return "HTTP connection is disconnected";
    if (connection_) return std::nullopt;
    try {
      connection_ = std::make_unique<CurlLuaSkinHttpConnection>(url_, timeout_, limits_, stop_);
      return connection_->connect();
    } catch (...) { return "HTTP connection allocation failed"; }
  }
  LuaSkinHttpCodeResult responseCode() noexcept override {
    try {
      if (failure_) return {.failure = failure_};
      if (code_) return {.code = code_};
      for (;;) {
        if (auto failure = connect()) { failure_ = *failure; return {.failure = failure_}; }
        auto result = connection_->responseCode();
        if (result.failure || !result.code) return result;
        const int status = *result.code;
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
          code_ = status;
          return result;
        }
        if (connection_->redirectLocation().empty()) {
          failure_ = "Invalid or excessive HTTP redirects";
          return {.failure = failure_};
        }
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
        if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url_.c_str(), 0) != CURLUE_OK ||
            curl_url_set(parsed.get(), CURLUPART_URL, connection_->redirectLocation().c_str(), 0) != CURLUE_OK) {
          failure_ = "Invalid HTTP redirect";
          return {.failure = failure_};
        }
        char *text = nullptr;
        if (curl_url_get(parsed.get(), CURLUPART_URL, &text, 0) != CURLUE_OK) {
          failure_ = "Invalid HTTP redirect";
          return {.failure = failure_};
        }
        std::unique_ptr<char, decltype(&curl_free)> textGuard(text, curl_free);
        std::string next(text);
        if (auto invalid = LuaSkinHttpClient::validateAsyncRedirect(url_, next, ++redirects_)) {
          failure_ = *invalid; return {.failure = failure_};
        }
        connection_.reset();
        url_ = std::move(next);
      }
    } catch (...) { return {.failure = "HTTP redirect processing failed"}; }
  }
  LuaSkinHttpBodyResult readBody() noexcept override {
    const auto code = responseCode();
    if (code.failure) return {.failure = code.failure};
    return connection_->readBody();
  }
  void setResponseLimitBytes(std::size_t bytes) noexcept override {
    if (bytes < 1 || bytes > LuaSkinHttpClient::maximumBytes) return;
    limits_.maximumBytes = bytes;
    if (connection_) connection_->setResponseLimitBytes(bytes);
  }
  void disconnect() noexcept override { disconnected_ = true; connection_.reset(); }
private:
  std::string url_;
  int timeout_;
  LuaSkinHttpLimits limits_;
  std::stop_token stop_;
  std::unique_ptr<CurlLuaSkinHttpConnection> connection_;
  std::optional<std::string> failure_;
  std::optional<int> code_;
  int redirects_ = 0;
  bool disconnected_ = false;
};

class CurlLuaSkinHttpTransport final : public LuaSkinHttpTransport {
public:
  explicit CurlLuaSkinHttpTransport(std::stop_token stop) : stop_(stop) {}

  LuaSkinHttpOpenResult open(std::string_view url, int timeoutMilliseconds,
                             LuaSkinHttpLimits limits) override {
    try {
      if (limits.restrictedDestinations) {
        return {.connection = std::make_unique<RestrictedCurlHttpConnection>(
            std::string(url), timeoutMilliseconds, limits, stop_)};
      }
      return {.connection = std::make_unique<CurlLuaSkinHttpConnection>(
                  std::string(url), timeoutMilliseconds, limits, stop_)};
    } catch (...) {
      return {.failure = "libcurl connection allocation failed"};
    }
  }

private:
  std::stop_token stop_;
};

} // namespace

#if defined(ASOBMASHOW_TEST_CURL_HTTP_TRANSPORT)
void setLuaSkinCurlDnsServersForTesting(std::string servers) {
  std::lock_guard lock(testDnsMutex);
  testDnsServers = std::move(servers);
}
#endif

std::unique_ptr<LuaSkinHttpTransport>
createLuaSkinProductionHttpTransport(std::stop_token stop) {
  try {
    return std::make_unique<CurlLuaSkinHttpTransport>(stop);
  } catch (...) {
    return nullptr;
  }
}

} // namespace skin

#endif
