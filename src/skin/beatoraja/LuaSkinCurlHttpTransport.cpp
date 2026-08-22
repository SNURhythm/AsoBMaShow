#include "LuaSkinCurlHttpTransport.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(__APPLE__) || !TARGET_OS_IPHONE

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
    if (stop_.stop_requested()) {
      return "HTTP request was cancelled";
    }
    if (!initialize()) {
      return initializationFailure_;
    }
    if (curl_easy_setopt(curl_.get(), CURLOPT_CONNECT_ONLY, 1L) != CURLE_OK) {
      return "libcurl connect stage could not be configured";
    }
    error_.fill('\0');
    const CURLcode result = curl_easy_perform(curl_.get());
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

private:
  enum class TransferPhase { Response, Read };

  bool initialize() noexcept {
    if (curl_) {
      return true;
    }
    if (!initializeCurl()) {
      initializationFailure_ = "libcurl initialization failed";
      return false;
    }
    curl_.reset(curl_easy_init());
    if (!curl_) {
      initializationFailure_ = "libcurl connection allocation failed";
      return false;
    }
    error_.fill('\0');
    const bool configured =
        curl_easy_setopt(curl_.get(), CURLOPT_URL, url_.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_HTTPGET, 1L) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_HTTP_VERSION,
                         CURL_HTTP_VERSION_1_1) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(timeoutMilliseconds_)) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_TIMEOUT_MS,
                         static_cast<long>(timeoutMilliseconds_)) == CURLE_OK &&
        curl_easy_setopt(curl_.get(), CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
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
    ConfigureCurlTrustStore(curl_.get());
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
    lineSeparators_ = 0;
    utf16Characters_ = 0;
    pendingSize_ = 0;
    previousWasCarriageReturn_ = false;
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
        curl_easy_setopt(curl_.get(), CURLOPT_FAILONERROR, 1L) == CURLE_OK;
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
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMilliseconds_);
    while (true) {
      if (stop_.stop_requested()) {
        return "HTTP request was cancelled";
      }
      int running = 0;
      const CURLMcode performed = curl_multi_perform(multi_, &running);
      if (performed != CURLM_OK) {
        return curl_multi_strerror(performed);
      }
      collectCompletion();
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
    flushPending();
    if (allocationFailed_) {
      readFailure_ = "HTTP response allocation failed";
    } else if (tooLarge_) {
      readFailure_ = "response is too large";
    } else if (stop_.stop_requested() ||
               transferResult_ == CURLE_ABORTED_BY_CALLBACK) {
      readFailure_ = "HTTP request was cancelled";
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
    } else if (isBlankHeader(line)) {
      const bool informational = self->currentResponseStatus_ >= 100 &&
                                 self->currentResponseStatus_ < 200;
      const bool redirect = self->currentResponseStatus_ >= 300 &&
                            self->currentResponseStatus_ < 400 &&
                            self->currentResponseHasLocation_;
      if (!informational && !redirect) {
        if (curl_easy_pause(self->curl_.get(), CURLPAUSE_RECV) != CURLE_OK) {
          return 0;
        }
        self->responseHeadersReady_ = true;
        self->responsePaused_ = true;
      }
    }
    return bytes;
  }

  void addCharacters(std::size_t count) noexcept {
    if (utf16Characters_ > limits_.maximumCharacters ||
        count > limits_.maximumCharacters - utf16Characters_) {
      tooLarge_ = true;
      intentionalStop_ = true;
      return;
    }
    utf16Characters_ += count;
  }

  void flushPending() noexcept {
    if (pendingSize_ != 0) {
      addCharacters(1);
      pendingSize_ = 0;
    }
  }

  void countUtf8(unsigned char value) noexcept {
    if (pendingSize_ == 0) {
      if (value <= 0x7f) {
        addCharacters(1);
        return;
      }
      if (value >= 0xc2 && value <= 0xdf) {
        pendingExpected_ = 2;
      } else if (value >= 0xe0 && value <= 0xef) {
        pendingExpected_ = 3;
      } else if (value >= 0xf0 && value <= 0xf4) {
        pendingExpected_ = 4;
      } else {
        addCharacters(1);
        return;
      }
      pending_[0] = value;
      pendingSize_ = 1;
      return;
    }
    if ((value & 0xc0) != 0x80) {
      flushPending();
      countUtf8(value);
      return;
    }
    pending_[pendingSize_++] = value;
    if (pendingSize_ != pendingExpected_) {
      return;
    }
    std::uint32_t codepoint =
        pending_[0] & (pendingExpected_ == 2   ? 0x1fU
                       : pendingExpected_ == 3 ? 0x0fU
                                               : 0x07U);
    for (std::size_t index = 1; index < pendingSize_; ++index) {
      codepoint = (codepoint << 6) | (pending_[index] & 0x3fU);
    }
    const bool valid =
        !((pendingExpected_ == 3 && codepoint < 0x800) ||
          (pendingExpected_ == 4 && codepoint < 0x10000) ||
          (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
          codepoint > 0x10ffff);
    const std::size_t decodedCharacters =
        valid ? (codepoint > 0xffff ? 2U : 1U) : pendingExpected_;
    pendingSize_ = 0;
    addCharacters(decodedCharacters);
  }

  std::size_t consume(const char *bytes, std::size_t size) noexcept {
    try {
      body_.append(bytes, size);
    } catch (...) {
      allocationFailed_ = true;
      intentionalStop_ = true;
      return 0;
    }
    for (std::size_t index = 0; index < size && !intentionalStop_; ++index) {
      const unsigned char value = static_cast<unsigned char>(bytes[index]);
      if (value == '\r' || value == '\n') {
        flushPending();
        if (value == '\n' && previousWasCarriageReturn_) {
          previousWasCarriageReturn_ = false;
          continue;
        }
        previousWasCarriageReturn_ = value == '\r';
        ++lineSeparators_;
        if (lineSeparators_ >= limits_.maximumLines) {
          intentionalStop_ = true;
        }
      } else {
        previousWasCarriageReturn_ = false;
        countUtf8(value);
      }
    }
    return intentionalStop_ ? 0 : size;
  }

  static std::size_t write(char *data, std::size_t size,
                           std::size_t count, void *opaque) noexcept {
    auto *self = static_cast<CurlLuaSkinHttpConnection *>(opaque);
    if (self == nullptr || data == nullptr || size == 0 ||
        count > std::numeric_limits<std::size_t>::max() / size) {
      return 0;
    }
    return self->consume(data, size * count);
  }

  static int progress(void *opaque, curl_off_t, curl_off_t, curl_off_t,
                      curl_off_t) noexcept {
    const auto *self = static_cast<CurlLuaSkinHttpConnection *>(opaque);
    return self != nullptr && self->stop_.stop_requested() ? 1 : 0;
  }

  std::string url_;
  int timeoutMilliseconds_ = 1000;
  LuaSkinHttpLimits limits_;
  std::stop_token stop_;
  CurlEasyHandle curl_{nullptr};
  CURLM *multi_ = nullptr;
  std::array<char, CURL_ERROR_SIZE> error_{};
  std::string initializationFailure_;
  std::string body_;
  std::optional<std::string> responseFailure_;
  std::optional<std::string> readFailure_;
  std::array<unsigned char, 4> pending_{};
  std::size_t pendingSize_ = 0;
  std::size_t pendingExpected_ = 0;
  std::size_t lineSeparators_ = 0;
  std::size_t utf16Characters_ = 0;
  int responseCode_ = 0;
  int currentResponseStatus_ = 0;
  bool connected_ = false;
  bool responseStarted_ = false;
  bool responseHeadersReady_ = false;
  bool responsePaused_ = false;
  bool currentResponseHasLocation_ = false;
  bool transferCompleted_ = false;
  bool readFinished_ = false;
  bool multiContainsEasy_ = false;
  bool disconnected_ = false;
  bool previousWasCarriageReturn_ = false;
  bool intentionalStop_ = false;
  bool tooLarge_ = false;
  bool allocationFailed_ = false;
  CURLcode transferResult_ = CURLE_OK;
};

class CurlLuaSkinHttpTransport final : public LuaSkinHttpTransport {
public:
  explicit CurlLuaSkinHttpTransport(std::stop_token stop) : stop_(stop) {}

  LuaSkinHttpOpenResult open(std::string_view url, int timeoutMilliseconds,
                             LuaSkinHttpLimits limits) override {
    try {
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
