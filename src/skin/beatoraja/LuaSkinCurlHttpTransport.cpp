#include "LuaSkinCurlHttpTransport.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(__APPLE__) || !TARGET_OS_IPHONE

#include "../../CurlRAII.h"

#include <curl/curl.h>

#include <array>
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
    if (auto failure = performResponse()) {
      return {.failure = std::move(failure)};
    }
    return {.code = responseCode_};
  }

  LuaSkinHttpBodyResult readBody() noexcept override {
    if (auto failure = connect()) {
      return {.failure = std::move(failure)};
    }
    if (auto failure = performResponse()) {
      return {.failure = std::move(failure)};
    }
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
    curl_.reset();
  }

private:
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

  std::optional<std::string> performResponse() noexcept {
    if (responsePerformed_) {
      return responseFailure_;
    }
    responsePerformed_ = true;
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
        curl_easy_setopt(curl_.get(), CURLOPT_FAILONERROR, 1L) == CURLE_OK;
    if (!configured) {
      responseFailure_ = "libcurl response stage could not be configured";
      return responseFailure_;
    }

    const CURLcode result = curl_easy_perform(curl_.get());
    long response = 0;
    const CURLcode info =
        curl_easy_getinfo(curl_.get(), CURLINFO_RESPONSE_CODE, &response);
    if (info != CURLE_OK || response <= 0 ||
        response > std::numeric_limits<int>::max()) {
      responseFailure_ = result == CURLE_OK
                             ? "HTTP response code is unavailable"
                             : curlFailure(result, error_);
      return responseFailure_;
    }
    responseCode_ = static_cast<int>(response);
    flushPending();
    if (allocationFailed_) {
      readFailure_ = "HTTP response allocation failed";
    } else if (tooLarge_) {
      readFailure_ = "response is too large";
    } else if (stop_.stop_requested() || result == CURLE_ABORTED_BY_CALLBACK) {
      readFailure_ = "HTTP request was cancelled";
    } else if (result != CURLE_OK && !intentionalStop_) {
      readFailure_ = curlFailure(result, error_);
    }
    return std::nullopt;
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
  bool connected_ = false;
  bool responsePerformed_ = false;
  bool disconnected_ = false;
  bool previousWasCarriageReturn_ = false;
  bool intentionalStop_ = false;
  bool tooLarge_ = false;
  bool allocationFailed_ = false;
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
