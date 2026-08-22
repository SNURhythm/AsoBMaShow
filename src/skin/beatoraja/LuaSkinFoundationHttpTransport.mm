#include "LuaSkinCurlHttpTransport.h"

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#if TARGET_OS_IPHONE

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace skin {
namespace {

struct FoundationHttpState {
  explicit FoundationHttpState(LuaSkinHttpLimits value) : limits(value) {}

  bool append(const void *bytes, std::size_t size) noexcept {
    try {
      body.append(static_cast<const char *>(bytes), size);
    } catch (...) {
      allocationFailed = true;
      intentionalStop = true;
      return false;
    }
    const auto *input = static_cast<const unsigned char *>(bytes);
    for (std::size_t index = 0; index < size && !intentionalStop; ++index) {
      const unsigned char value = input[index];
      if (value == '\r' || value == '\n') {
        flushPending();
        if (value == '\n' && previousWasCarriageReturn) {
          previousWasCarriageReturn = false;
          continue;
        }
        previousWasCarriageReturn = value == '\r';
        if (++lineSeparators >= limits.maximumLines) {
          intentionalStop = true;
        }
      } else {
        previousWasCarriageReturn = false;
        countUtf8(value);
      }
    }
    return !intentionalStop;
  }

  void finish() noexcept { flushPending(); }

  void addCharacters(std::size_t count) noexcept {
    if (utf16Characters > limits.maximumCharacters ||
        count > limits.maximumCharacters - utf16Characters) {
      tooLarge = true;
      intentionalStop = true;
      return;
    }
    utf16Characters += count;
  }

  void flushPending() noexcept {
    if (pendingSize != 0) {
      addCharacters(1);
      pendingSize = 0;
    }
  }

  void countUtf8(unsigned char value) noexcept {
    if (pendingSize == 0) {
      if (value <= 0x7f) {
        addCharacters(1);
        return;
      }
      if (value >= 0xc2 && value <= 0xdf) {
        pendingExpected = 2;
      } else if (value >= 0xe0 && value <= 0xef) {
        pendingExpected = 3;
      } else if (value >= 0xf0 && value <= 0xf4) {
        pendingExpected = 4;
      } else {
        addCharacters(1);
        return;
      }
      pending[0] = value;
      pendingSize = 1;
      return;
    }
    if ((value & 0xc0) != 0x80) {
      flushPending();
      countUtf8(value);
      return;
    }
    pending[pendingSize++] = value;
    if (pendingSize != pendingExpected) {
      return;
    }
    std::uint32_t codepoint =
        pending[0] & (pendingExpected == 2   ? 0x1fU
                      : pendingExpected == 3 ? 0x0fU
                                             : 0x07U);
    for (std::size_t index = 1; index < pendingSize; ++index) {
      codepoint = (codepoint << 6) | (pending[index] & 0x3fU);
    }
    const bool valid =
        !((pendingExpected == 3 && codepoint < 0x800) ||
          (pendingExpected == 4 && codepoint < 0x10000) ||
          (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
          codepoint > 0x10ffff);
    const std::size_t decodedCharacters =
        valid ? (codepoint > 0xffff ? 2U : 1U) : pendingExpected;
    pendingSize = 0;
    addCharacters(decodedCharacters);
  }

  LuaSkinHttpLimits limits;
  std::string body;
  std::string failure;
  std::array<unsigned char, 4> pending{};
  std::size_t pendingSize = 0;
  std::size_t pendingExpected = 0;
  std::size_t lineSeparators = 0;
  std::size_t utf16Characters = 0;
  std::atomic_bool responseSignalled{false};
  std::atomic_bool completionSignalled{false};
  int responseCode = 0;
  bool previousWasCarriageReturn = false;
  bool intentionalStop = false;
  bool tooLarge = false;
  bool allocationFailed = false;
};

} // namespace
} // namespace skin

@interface AsoLuaSkinHttpDelegate
    : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate> {
 @public
  std::shared_ptr<skin::FoundationHttpState> state;
  dispatch_semaphore_t responseSemaphore;
  dispatch_semaphore_t completionSemaphore;
}
@end

@implementation AsoLuaSkinHttpDelegate
- (void)URLSession:(NSURLSession *)session
              dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveResponse:(NSURLResponse *)response
     completionHandler:
         (void (^)(NSURLSessionResponseDisposition disposition))handler {
  (void)session;
  NSHTTPURLResponse *http =
      [response isKindOfClass:[NSHTTPURLResponse class]]
          ? static_cast<NSHTTPURLResponse *>(response)
          : nil;
  if (http == nil) {
    state->failure = "HTTP response is unavailable";
    handler(NSURLSessionResponseCancel);
  } else {
    state->responseCode = static_cast<int>(http.statusCode);
    handler(NSURLSessionResponseAllow);
    [dataTask suspend];
  }
  if (!state->responseSignalled.exchange(true)) {
    dispatch_semaphore_signal(responseSemaphore);
  }
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveData:(NSData *)data {
  (void)session;
  if (!state->append(data.bytes, data.length)) {
    [dataTask cancel];
  }
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:
                 (void (^)(NSURLRequest *_Nullable))handler {
  (void)session;
  (void)task;
  (void)response;
  NSString *scheme = request.URL.scheme.lowercaseString;
  if ([scheme isEqualToString:@"http"] ||
      [scheme isEqualToString:@"https"]) {
    handler(request);
  } else {
    state->failure = "unsupported redirect scheme";
    handler(nil);
  }
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
  (void)session;
  (void)task;
  state->finish();
  if (error != nil && !state->intentionalStop) {
    const char *message = error.localizedDescription.UTF8String;
    state->failure = message != nullptr ? message : "Foundation HTTP failed";
  }
  if (!state->responseSignalled.exchange(true)) {
    dispatch_semaphore_signal(responseSemaphore);
  }
  if (!state->completionSignalled.exchange(true)) {
    dispatch_semaphore_signal(completionSemaphore);
  }
}
@end

namespace skin {
namespace {

class FoundationLuaSkinHttpConnection final : public LuaSkinHttpConnection {
public:
  FoundationLuaSkinHttpConnection(std::string url, int timeoutMilliseconds,
                                  LuaSkinHttpLimits limits,
                                  std::stop_token stop)
      : url_(std::move(url)), timeoutMilliseconds_(timeoutMilliseconds),
        stop_(stop), state_(std::make_shared<FoundationHttpState>(limits)) {}

  ~FoundationLuaSkinHttpConnection() override { disconnect(); }

  std::optional<std::string> connect() noexcept override {
    @autoreleasepool {
      if (connected_) {
        return std::nullopt;
      }
      if (disconnected_) {
        return "HTTP connection is disconnected";
      }
      if (stop_.stop_requested()) {
        return "HTTP request was cancelled";
      }
      NSString *text = [[NSString alloc]
          initWithBytes:url_.data()
                 length:url_.size()
               encoding:NSUTF8StringEncoding];
      NSURL *url = text != nil ? [NSURL URLWithString:text] : nil;
      if (url == nil) {
        return "Foundation HTTP URL is invalid";
      }
      const NSTimeInterval timeout =
          static_cast<NSTimeInterval>(timeoutMilliseconds_) / 1000.0;
      NSMutableURLRequest *request = [NSMutableURLRequest
           requestWithURL:url
              cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
          timeoutInterval:timeout];
      request.HTTPMethod = @"GET";
      [request setValue:@"AsoBMaShow" forHTTPHeaderField:@"User-Agent"];
      delegate_ = [[AsoLuaSkinHttpDelegate alloc] init];
      delegate_->state = state_;
      delegate_->responseSemaphore = dispatch_semaphore_create(0);
      delegate_->completionSemaphore = dispatch_semaphore_create(0);
      NSURLSessionConfiguration *configuration =
          [NSURLSessionConfiguration ephemeralSessionConfiguration];
      configuration.requestCachePolicy =
          NSURLRequestReloadIgnoringLocalCacheData;
      configuration.timeoutIntervalForRequest = timeout;
      configuration.timeoutIntervalForResource = timeout;
      queue_ = [[NSOperationQueue alloc] init];
      queue_.maxConcurrentOperationCount = 1;
      session_ = [NSURLSession sessionWithConfiguration:configuration
                                                delegate:delegate_
                                           delegateQueue:queue_];
      task_ = [session_ dataTaskWithRequest:request];
      [task_ resume];
      if (auto failure = wait(delegate_->responseSemaphore)) {
        [task_ cancel];
        return failure;
      }
      if (!state_->failure.empty() && state_->responseCode == 0) {
        return state_->failure;
      }
      if (state_->responseCode == 0) {
        return "HTTP response is unavailable";
      }
      connected_ = true;
      return std::nullopt;
    }
  }

  LuaSkinHttpCodeResult responseCode() noexcept override {
    if (auto failure = connect()) {
      return {.failure = std::move(failure)};
    }
    return {.code = state_->responseCode};
  }

  LuaSkinHttpBodyResult readBody() noexcept override {
    @autoreleasepool {
      if (auto failure = connect()) {
        return {.failure = std::move(failure)};
      }
      if (!state_->completionSignalled.load()) {
        [task_ resume];
        if (auto failure = wait(delegate_->completionSemaphore)) {
          [task_ cancel];
          return {.failure = std::move(failure)};
        }
      }
      if (state_->allocationFailed) {
        return {.failure = "HTTP response allocation failed"};
      }
      if (state_->tooLarge) {
        return {.failure = "response is too large"};
      }
      if (state_->responseCode >= 400) {
        return {.failure = "HTTP " + std::to_string(state_->responseCode)};
      }
      if (!state_->failure.empty() && !state_->intentionalStop) {
        return {.failure = state_->failure};
      }
      return {.body = state_->body};
    }
  }

  void disconnect() noexcept override {
    @autoreleasepool {
      if (disconnected_) {
        return;
      }
      disconnected_ = true;
      [task_ cancel];
      [session_ invalidateAndCancel];
      task_ = nil;
      session_ = nil;
      delegate_ = nil;
      queue_ = nil;
    }
  }

private:
  std::optional<std::string>
  wait(dispatch_semaphore_t semaphore) noexcept {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMilliseconds_) +
                          std::chrono::seconds(1);
    while (dispatch_semaphore_wait(
               semaphore,
               dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC)) != 0) {
      if (stop_.stop_requested()) {
        return "HTTP request was cancelled";
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return "HTTP request timed out";
      }
    }
    return std::nullopt;
  }

  std::string url_;
  int timeoutMilliseconds_ = 1000;
  std::stop_token stop_;
  std::shared_ptr<FoundationHttpState> state_;
  __strong AsoLuaSkinHttpDelegate *delegate_ = nil;
  __strong NSURLSession *session_ = nil;
  __strong NSURLSessionDataTask *task_ = nil;
  __strong NSOperationQueue *queue_ = nil;
  bool connected_ = false;
  bool disconnected_ = false;
};

class FoundationLuaSkinHttpTransport final : public LuaSkinHttpTransport {
public:
  explicit FoundationLuaSkinHttpTransport(std::stop_token stop) : stop_(stop) {}

  LuaSkinHttpOpenResult open(std::string_view url, int timeoutMilliseconds,
                             LuaSkinHttpLimits limits) override {
    try {
      return {.connection =
                  std::make_unique<FoundationLuaSkinHttpConnection>(
                      std::string(url), timeoutMilliseconds, limits, stop_)};
    } catch (...) {
      return {.failure = "Foundation HTTP connection allocation failed"};
    }
  }

private:
  std::stop_token stop_;
};

} // namespace

std::unique_ptr<LuaSkinHttpTransport>
createLuaSkinProductionHttpTransport(std::stop_token stop) {
  try {
    return std::make_unique<FoundationLuaSkinHttpTransport>(stop);
  } catch (...) {
    return nullptr;
  }
}

} // namespace skin

#endif
