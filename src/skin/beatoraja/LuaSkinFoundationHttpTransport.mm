#include "LuaSkinCurlHttpTransport.h"

#import <Foundation/Foundation.h>
#import <CFNetwork/CFNetwork.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#import <TargetConditionals.h>

#if TARGET_OS_IPHONE || defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)

#include "LuaSkinFoundationHttpTransportTest.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <algorithm>
#include <string>
#include <utility>

namespace skin {
namespace {

struct FoundationHttpState {
  explicit FoundationHttpState(LuaSkinHttpLimits value) : limits(std::move(value)) {}

  bool append(const void *bytes, std::size_t size) noexcept {
    if (size > limits.maximumBytes - body.size()) {
      tooLarge = true;
      intentionalStop = true;
      return false;
    }
    try { body.append(static_cast<const char *>(bytes), size); }
    catch (...) { allocationFailed = true; intentionalStop = true; return false; }
    return true;
  }

  std::mutex mutex;
  LuaSkinHttpLimits limits;
  std::string body;
  std::string failure;
  int redirects = 0;
  std::atomic_bool responseSignalled{false};
  std::atomic_bool completionSignalled{false};
  int responseCode = 0;
  long long expectedBytes = -1;
  void (^responseDecision)(NSURLSessionResponseDisposition) = nil;
  bool closed = false;
  bool intentionalStop = false;
  bool tooLarge = false;
  bool allocationFailed = false;
};

void resolveFoundationResponseDecision(FoundationHttpState &state,
    NSURLSessionResponseDisposition disposition) noexcept {
  void (^decision)(NSURLSessionResponseDisposition) = nil;
  {
    std::lock_guard lock(state.mutex);
    if (state.closed) disposition = NSURLSessionResponseCancel;
    if (disposition == NSURLSessionResponseCancel) state.closed = true;
    decision = state.responseDecision;
    state.responseDecision = nil;
  }
  if (decision) decision(disposition);
}

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
  (void)dataTask;
  bool cancel = false;
  {
    std::lock_guard lock(state->mutex);
    NSHTTPURLResponse *http = [response isKindOfClass:[NSHTTPURLResponse class]]
        ? static_cast<NSHTTPURLResponse *>(response) : nil;
    if (http == nil || state->closed) {
      if (http == nil) state->failure = "HTTP response is unavailable";
      cancel = true;
    } else {
      state->responseCode = static_cast<int>(http.statusCode);
      state->expectedBytes = response.expectedContentLength;
      // Do not permit body callbacks until the caller chooses its final limit.
      state->responseDecision = [handler copy];
    }
  }
  if (cancel) handler(NSURLSessionResponseCancel);
  if (!state->responseSignalled.exchange(true)) {
    dispatch_semaphore_signal(responseSemaphore);
  }
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveData:(NSData *)data {
  (void)session;
  std::lock_guard lock(state->mutex);
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
  std::optional<std::string> failure;
  NSString *scheme = request.URL.scheme.lowercaseString;
  if (![scheme isEqualToString:@"http"] && ![scheme isEqualToString:@"https"]) {
    failure = "unsupported redirect scheme";
  }
  if (failure) {
    { std::lock_guard lock(state->mutex); state->failure = *failure; }
    handler(nil);
  } else { handler(request); }
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
  (void)session;
  (void)task;
  {
    std::lock_guard lock(state->mutex);
    state->closed = true;
    if (error != nil && !state->intentionalStop && state->failure.empty()) {
      const char *message = error.localizedDescription.UTF8String;
      state->failure = message != nullptr ? message : "Foundation HTTP failed";
    }
  }
  skin::resolveFoundationResponseDecision(*state, NSURLSessionResponseCancel);
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
      if (auto failure = stopped()) return failure;
      NSString *text = [[NSString alloc]
          initWithBytes:url_.data()
                 length:url_.size()
               encoding:NSUTF8StringEncoding];
      NSURL *url = text != nil ? [NSURL URLWithString:text] : nil;
      if (url == nil) {
        return "Foundation HTTP URL is invalid";
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          state_->limits.deadline - std::chrono::steady_clock::now()).count();
      const NSTimeInterval timeout = static_cast<NSTimeInterval>(
          std::max<std::int64_t>(1, std::min<std::int64_t>(timeoutMilliseconds_, remaining))) / 1000.0;
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
        resolveResponseDecision(NSURLSessionResponseCancel);
        [task_ cancel];
        return failure;
      }
      std::lock_guard lock(state_->mutex);
      if (!state_->failure.empty()) {
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
    std::lock_guard lock(state_->mutex);
    return {.code = state_->responseCode};
  }

  LuaSkinHttpBodyResult readBody() noexcept override {
    @autoreleasepool {
      if (auto failure = connect()) {
        return {.failure = std::move(failure)};
      }
      if (auto failure = stopped()) {
        resolveResponseDecision(NSURLSessionResponseCancel);
        [task_ cancel];
        return {.failure = failure};
      }
      bool tooLarge = false;
      {
        std::lock_guard lock(state_->mutex);
        tooLarge = (state_->expectedBytes > 0 &&
            static_cast<unsigned long long>(state_->expectedBytes) > state_->limits.maximumBytes) ||
            state_->body.size() > state_->limits.maximumBytes;
      }
      if (tooLarge) {
        resolveResponseDecision(NSURLSessionResponseCancel);
        [task_ cancel];
        return {.failure = "response is too large"};
      }
      resolveResponseDecision(NSURLSessionResponseAllow);
      if (!state_->completionSignalled.load()) {
        if (auto failure = wait(delegate_->completionSemaphore)) {
          [task_ cancel];
          return {.failure = std::move(failure)};
        }
      }
      std::lock_guard lock(state_->mutex);
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

  void setResponseLimitBytes(std::size_t bytes) noexcept override {
    if (bytes < 1 || bytes > LuaSkinHttpClient::maximumBytes) return;
    std::lock_guard lock(state_->mutex);
    state_->limits.maximumBytes = bytes;
  }

  void disconnect() noexcept override {
    @autoreleasepool {
      if (disconnected_) {
        return;
      }
      disconnected_ = true;
      resolveResponseDecision(NSURLSessionResponseCancel);
      [task_ cancel];
      [session_ invalidateAndCancel];
      task_ = nil;
      session_ = nil;
      delegate_ = nil;
      queue_ = nil;
    }
  }

private:
  void resolveResponseDecision(NSURLSessionResponseDisposition disposition) noexcept {
    resolveFoundationResponseDecision(*state_, disposition);
  }
  std::optional<std::string> stopped() const {
    if (stop_.stop_requested() || state_->limits.requestStop.stop_requested()) return "HTTP request was cancelled";
    if (std::chrono::steady_clock::now() >= state_->limits.deadline) return "HTTP request timed out";
    return std::nullopt;
  }
  std::optional<std::string>
  wait(dispatch_semaphore_t semaphore) noexcept {
    const auto deadline = std::min(state_->limits.deadline, std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMilliseconds_));
    while (dispatch_semaphore_wait(
               semaphore,
               dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC)) != 0) {
      if (auto failure = stopped()) return failure;
      if (std::chrono::steady_clock::now() >= deadline) {
        return "HTTP request timed out";
      }
    }
    return stopped();
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

// CFHTTPStream is intentionally used for the restricted path: NSURLSession
// cannot pin a resolved peer while retaining the original TLS server name.
// DNS and stream events are scheduled only on the calling HTTP worker's run loop.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
template<class T> struct FoundationCFHandle {
  T value = nullptr;
  explicit FoundationCFHandle(T initial = nullptr) : value(initial) {}
  ~FoundationCFHandle() { if (value) CFRelease(value); }
  FoundationCFHandle(const FoundationCFHandle &) = delete;
  FoundationCFHandle &operator=(const FoundationCFHandle &) = delete;
};

std::string foundationText(NSString *text) {
  const char *value = text.UTF8String;
  return value ? value : "";
}

std::optional<std::string> configureFoundationPinnedRequest(
    NSURLComponents *components, NSString *host, const std::string &numeric,
    CFHTTPMessageRef &request, CFReadStreamRef &stream) {
  NSString *authority = [host containsString:@":"] ? [NSString stringWithFormat:@"[%@]", host] : host;
  if (components.port) authority = [NSString stringWithFormat:@"%@:%@", authority, components.port];
  components.host = numeric.find(':') == std::string::npos ?
      [NSString stringWithUTF8String:numeric.c_str()] :
      [NSString stringWithFormat:@"[%s]", numeric.c_str()];
  NSURL *pinnedURL = components.URL;
  if (!pinnedURL) return "Invalid pinned HTTP destination";
  request = CFHTTPMessageCreateRequest(nullptr,
      CFSTR("GET"), (__bridge CFURLRef)pinnedURL, kCFHTTPVersion1_1);
  if (!request) return "HTTP request allocation failed";
  CFHTTPMessageSetHeaderFieldValue(request, CFSTR("Host"), (__bridge CFStringRef)authority);
  CFHTTPMessageSetHeaderFieldValue(request, CFSTR("User-Agent"), CFSTR("AsoBMaShow"));
  CFHTTPMessageSetHeaderFieldValue(request, CFSTR("Connection"), CFSTR("close"));
  stream = CFReadStreamCreateForHTTPRequest(nullptr, request);
  if (!stream) return "HTTP stream allocation failed";
  if (!CFReadStreamSetProperty(stream, kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanFalse) ||
      !CFReadStreamSetProperty(stream, kCFStreamPropertyHTTPProxy, (__bridge CFDictionaryRef)@{}) ||
      !CFReadStreamSetProperty(stream, kCFStreamPropertySOCKSProxy, (__bridge CFDictionaryRef)@{}))
    return "HTTP destination policy could not be configured";
  if ([components.scheme.lowercaseString isEqualToString:@"https"]) {
    NSDictionary *tls = @{(__bridge NSString *)kCFStreamSSLPeerName: host};
    if (!CFReadStreamSetProperty(stream, kCFStreamPropertySSLSettings, (__bridge CFDictionaryRef)tls))
      return "HTTP TLS peer validation could not be configured";
  }
  return std::nullopt;
}

class RestrictedFoundationHttpConnection final : public LuaSkinHttpConnection {
public:
  RestrictedFoundationHttpConnection(std::string url, int timeout, LuaSkinHttpLimits limits, std::stop_token stop)
      : url_(std::move(url)), limits_(std::move(limits)), stop_(stop) {
    limits_.deadline = std::min(limits_.deadline, std::chrono::steady_clock::now() +
        std::chrono::milliseconds(LuaSkinHttpClient::clampTimeout(timeout)));
  }
#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
  RestrictedFoundationHttpConnection(std::string url, LuaSkinHttpLimits limits,
      LuaSkinFoundationCandidateTestOptions options)
      : RestrictedFoundationHttpConnection(std::move(url), 5000, std::move(limits), {}) {
    testOptions_ = std::move(options);
  }
#endif
  ~RestrictedFoundationHttpConnection() override { disconnect(); }
  std::optional<std::string> connect() noexcept override {
    @autoreleasepool {
      if (disconnected_) return "HTTP connection is disconnected";
      if (auto failure = stopped()) return failure;
      if (stream_) return std::nullopt;
      try { return openHop(); }
      catch (...) { return "Foundation HTTP connection failed"; }
    }
  }
  LuaSkinHttpCodeResult responseCode() noexcept override {
    @autoreleasepool {
      try {
        if (failure_) return {.failure = failure_};
        if (code_) return {.code = code_};
        for (;;) {
          if (auto failure = connect()) { failure_ = *failure; return {.failure = failure_}; }
          FoundationCFHandle<CFHTTPMessageRef> response;
          while (!response.value) {
            if (auto failure = stopped()) return {.failure = failure};
            response.value = static_cast<CFHTTPMessageRef>(
                const_cast<void *>(CFReadStreamCopyProperty(stream_, kCFStreamPropertyHTTPResponseHeader)));
            if (response.value) break;
            auto failure = streamFailure();
            if (!failure && CFReadStreamGetStatus(stream_) == kCFStreamStatusAtEnd)
              failure = "HTTP response headers unavailable";
            if (!failure && now() >= candidateDeadline_) failure = "HTTP request timed out";
            if (failure) {
              if (nextCandidate_ >= candidates_.size()) {
                failure_ = *failure;
                return {.failure = failure_};
              }
              if (auto nextFailure = openNextCandidate()) {
                failure_ = *nextFailure;
                return {.failure = failure_};
              }
              continue;
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
          }
          int status = static_cast<int>(CFHTTPMessageGetResponseStatusCode(response.value));
          if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
            code_ = status;
            FoundationCFHandle<CFStringRef> length(
                CFHTTPMessageCopyHeaderFieldValue(response.value, CFSTR("Content-Length")));
            if (length.value) {
              NSString *text = (__bridge NSString *)length.value;
              NSScanner *scanner = [NSScanner scannerWithString:text];
              unsigned long long bytes = 0;
              if ([scanner scanUnsignedLongLong:&bytes]) expectedBytes_ = bytes;
            }
            return {.code = code_};
          }
          FoundationCFHandle<CFStringRef> location(
              CFHTTPMessageCopyHeaderFieldValue(response.value, CFSTR("Location")));
          if (!location.value) return {.failure = "Invalid HTTP redirect"};
          NSURL *base = [NSURL URLWithString:[NSString stringWithUTF8String:url_.c_str()]];
          NSURL *nextURL = [NSURL URLWithString:(__bridge NSString *)location.value relativeToURL:base].absoluteURL;
          std::string next = foundationText(nextURL.absoluteString);
          if (auto invalid = LuaSkinHttpClient::validateAsyncRedirect(url_, next, ++redirects_)) {
            failure_ = *invalid; return {.failure = failure_};
          }
          closeStream();
          url_ = std::move(next);
        }
      } catch (...) { return {.failure = "Foundation HTTP response failed"}; }
    }
  }
  LuaSkinHttpBodyResult readBody() noexcept override {
    @autoreleasepool {
      try {
        if (auto code = responseCode(); code.failure) return {.failure = code.failure};
        if (expectedBytes_ > limits_.maximumBytes) return {.failure = "response is too large"};
        std::array<UInt8, 8192> buffer{};
        for (;;) {
          if (auto failure = stopped()) return {.failure = failure};
          if (auto failure = streamFailure()) return {.failure = failure};
          if (CFReadStreamHasBytesAvailable(stream_)) {
            const auto maximum = std::min(buffer.size(), limits_.maximumBytes - body_.size() + 1);
            const auto count = CFReadStreamRead(stream_, buffer.data(), static_cast<CFIndex>(maximum));
            if (count < 0) return {.failure = "Foundation HTTP read failed"};
            if (static_cast<std::size_t>(count) > limits_.maximumBytes - body_.size())
              return {.failure = "response is too large"};
            body_.append(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(count));
          } else if (CFReadStreamGetStatus(stream_) == kCFStreamStatusAtEnd) {
            return {.body = std::move(body_)};
          } else { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true); }
        }
      } catch (...) { return {.failure = "HTTP response allocation failed"}; }
    }
  }
  void setResponseLimitBytes(std::size_t bytes) noexcept override {
    if (bytes >= 1 && bytes <= LuaSkinHttpClient::maximumBytes) limits_.maximumBytes = bytes;
  }
  void disconnect() noexcept override { disconnected_ = true; closeStream(); }
private:
  std::chrono::steady_clock::time_point now() const {
#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
    if (testOptions_ && testOptions_->now) return testOptions_->now();
#endif
    return std::chrono::steady_clock::now();
  }
  std::optional<std::string> stopped() const {
    if (stop_.stop_requested() || limits_.requestStop.stop_requested()) return "HTTP request was cancelled";
    if (now() >= limits_.deadline) return "HTTP request timed out";
    return std::nullopt;
  }
  std::optional<std::string> streamFailure() const {
    if (CFReadStreamGetStatus(stream_) != kCFStreamStatusError) return std::nullopt;
    FoundationCFHandle<CFErrorRef> error(CFReadStreamCopyError(stream_));
    FoundationCFHandle<CFStringRef> description(error.value ? CFErrorCopyDescription(error.value) : nullptr);
    return description.value ? foundationText((__bridge NSString *)description.value) : "Foundation HTTP stream failed";
  }
  void closeStream() noexcept {
    if (!stream_) return;
    CFReadStreamSetClient(stream_, 0, nullptr, nullptr);
    CFReadStreamUnscheduleFromRunLoop(stream_, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFReadStreamClose(stream_);
    CFRelease(stream_);
    stream_ = nullptr;
  }
  std::optional<std::string> openHop() {
    if (auto invalid = LuaSkinHttpClient::validateAsyncUrl(url_)) return invalid;
    NSURLComponents *components = [NSURLComponents componentsWithString:[NSString stringWithUTF8String:url_.c_str()]];
    NSString *host = components.host;
    if ([host hasPrefix:@"["] && [host hasSuffix:@"]"])
      host = [host substringWithRange:NSMakeRange(1, host.length - 2)];
    if (host.length == 0) return "Invalid HTTP host";
    candidates_.clear();
    nextCandidate_ = 0;
#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
    if (testOptions_) candidates_ = testOptions_->candidates;
    else
#endif
    if (auto failure = resolveCandidates(host)) return failure;
    if (candidates_.empty()) return "HTTP DNS resolution failed";
    return openNextCandidate();
  }
  std::optional<std::string> resolveCandidates(NSString *host) {
    FoundationCFHandle<CFHostRef> resolver(CFHostCreateWithName(nullptr, (__bridge CFStringRef)host));
    if (!resolver.value) return "HTTP DNS allocation failed";
    struct Resolution { bool done = false; CFStreamError error{}; } resolution;
    CFHostClientContext context{0, &resolution, nullptr, nullptr, nullptr};
    if (!CFHostSetClient(resolver.value, [](CFHostRef, CFHostInfoType, const CFStreamError *error, void *opaque) {
      auto &value = *static_cast<Resolution *>(opaque);
      if (error) value.error = *error;
      value.done = true;
    }, &context)) return "HTTP DNS setup failed";
    CFHostScheduleWithRunLoop(resolver.value, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    struct Unschedule {
      CFHostRef host;
      ~Unschedule() {
        CFHostCancelInfoResolution(host, kCFHostAddresses);
        CFHostUnscheduleFromRunLoop(host, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        CFHostSetClient(host, nullptr, nullptr);
      }
    } unschedule{resolver.value};
    if (!CFHostStartInfoResolution(resolver.value, kCFHostAddresses, &resolution.error))
      return "HTTP DNS resolution failed";
    while (!resolution.done) {
      if (auto failure = stopped()) return failure;
      CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    if (resolution.error.error) return "HTTP DNS resolution failed";
    Boolean resolved = false;
    CFArrayRef addresses = CFHostGetAddressing(resolver.value, &resolved);
    if (!resolved || !addresses || CFArrayGetCount(addresses) == 0) return "HTTP DNS resolution failed";
    for (CFIndex index = 0; index < CFArrayGetCount(addresses); ++index) {
      auto data = static_cast<CFDataRef>(CFArrayGetValueAtIndex(addresses, index));
      if (CFDataGetLength(data) < sizeof(sockaddr)) return "Invalid HTTP DNS address";
      auto *address = reinterpret_cast<const sockaddr *>(CFDataGetBytePtr(data));
      std::span<const unsigned char> bytes;
      if (address->sa_family == AF_INET && CFDataGetLength(data) >= sizeof(sockaddr_in)) {
        bytes = {reinterpret_cast<const unsigned char *>(&reinterpret_cast<const sockaddr_in *>(address)->sin_addr), 4};
      } else if (address->sa_family == AF_INET6 && CFDataGetLength(data) >= sizeof(sockaddr_in6)) {
        bytes = {reinterpret_cast<const unsigned char *>(&reinterpret_cast<const sockaddr_in6 *>(address)->sin6_addr), 16};
      }
      if (!LuaSkinHttpClient::isPublicAddress(bytes)) return "Non-public HTTP destination denied";
      std::array<char, INET6_ADDRSTRLEN> text{};
      if (!inet_ntop(address->sa_family, bytes.data(), text.data(), text.size())) return "Invalid HTTP DNS address";
      candidates_.emplace_back(text.data());
    }
    return std::nullopt;
  }
  std::optional<std::string> openNextCandidate() {
    std::optional<std::string> failure;
    do {
      if (auto stoppedFailure = stopped()) return stoppedFailure;
      failure = openCandidate();
      // Configuration failures apply to every candidate. Only a failed native
      // connection open is retried here; asynchronous failures are handled while
      // awaiting headers, never after publishing a response status.
      if (!failure || !candidateConnectFailed_) return failure;
    } while (nextCandidate_ < candidates_.size());
    return failure;
  }
  std::optional<std::string> openCandidate() {
    closeStream();
    const auto index = nextCandidate_++;
#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
    if (testOptions_ && testOptions_->beforeAttempt) testOptions_->beforeAttempt(index);
#endif
    if (auto failure = stopped()) return failure;
    const auto started = now();
    // Give every remaining address a share of the original deadline, including
    // connect/TLS/header waits. A black hole cannot consume all fallback time.
    candidateDeadline_ = started + (limits_.deadline - started) /
        static_cast<std::chrono::steady_clock::duration::rep>(candidates_.size() - index);
    candidateConnectFailed_ = false;
    NSURLComponents *components = [NSURLComponents componentsWithString:[NSString stringWithUTF8String:url_.c_str()]];
    NSString *host = components.host;
    if ([host hasPrefix:@"["] && [host hasSuffix:@"]"])
      host = [host substringWithRange:NSMakeRange(1, host.length - 2)];
    const auto &numeric = candidates_[index];
    if (auto failure = stopped()) return failure;
    FoundationCFHandle<CFHTTPMessageRef> request;
    if (auto failure = configureFoundationPinnedRequest(components, host, numeric, request.value, stream_)) return failure;
    CFStreamClientContext streamContext{0, nullptr, nullptr, nullptr, nullptr};
    if (!CFReadStreamSetClient(stream_, kCFStreamEventHasBytesAvailable | kCFStreamEventErrorOccurred |
        kCFStreamEventEndEncountered, [](CFReadStreamRef, CFStreamEventType, void *) {}, &streamContext))
      return "HTTP stream scheduling failed";
    CFReadStreamScheduleWithRunLoop(stream_, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    if (!CFReadStreamOpen(stream_)) {
      candidateConnectFailed_ = true;
      return "HTTP connection failed";
    }
    return std::nullopt;
  }
  std::vector<std::string> candidates_;
  std::size_t nextCandidate_ = 0;
  std::chrono::steady_clock::time_point candidateDeadline_;
  bool candidateConnectFailed_ = false;
#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
  std::optional<LuaSkinFoundationCandidateTestOptions> testOptions_;
#endif
  std::string url_;
  LuaSkinHttpLimits limits_;
  std::stop_token stop_;
  CFReadStreamRef stream_ = nullptr;
  std::optional<int> code_;
  std::optional<std::string> failure_;
  std::string body_;
  int redirects_ = 0;
  unsigned long long expectedBytes_ = 0;
  bool disconnected_ = false;
};
#pragma clang diagnostic pop

class FoundationLuaSkinHttpTransport final : public LuaSkinHttpTransport {
public:
  explicit FoundationLuaSkinHttpTransport(std::stop_token stop) : stop_(stop) {}

  LuaSkinHttpOpenResult open(std::string_view url, int timeoutMilliseconds,
                             LuaSkinHttpLimits limits) override {
    try {
      if (limits.restrictedDestinations) {
        return {.connection = std::make_unique<RestrictedFoundationHttpConnection>(
            std::string(url), timeoutMilliseconds, limits, stop_)};
      }
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

LuaSkinFoundationAppendProbe
probeLuaSkinFoundationAppend(LuaSkinHttpLimits limits,
                             std::span<const std::byte> chunk) {
  FoundationHttpState state(limits);
  const bool continued = state.append(chunk.data(), chunk.size());
  return {.storedBytes = state.body.size(),
          .continued = continued,
          .tooLarge = state.tooLarge};
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

LuaSkinFoundationPinnedRequestProbe probeLuaSkinFoundationPinnedRequest(
    std::string_view url, std::string_view address) {
  @autoreleasepool {
    LuaSkinFoundationPinnedRequestProbe result;
    NSURLComponents *components = [NSURLComponents componentsWithString:
        [NSString stringWithUTF8String:std::string(url).c_str()]];
    FoundationCFHandle<CFHTTPMessageRef> request;
    FoundationCFHandle<CFReadStreamRef> stream;
    result.failure = configureFoundationPinnedRequest(components, components.host,
        std::string(address), request.value, stream.value);
    if (result.failure) return result;
    FoundationCFHandle<CFURLRef> effective(CFHTTPMessageCopyRequestURL(request.value));
    FoundationCFHandle<CFStringRef> host(CFHTTPMessageCopyHeaderFieldValue(request.value, CFSTR("Host")));
    result.url = foundationText([(__bridge NSURL *)effective.value absoluteString]);
    result.host = foundationText((__bridge NSString *)host.value);
    FoundationCFHandle<CFTypeRef> redirects(CFReadStreamCopyProperty(stream.value, kCFStreamPropertyHTTPShouldAutoredirect));
    result.redirectsDisabled = redirects.value && CFEqual(redirects.value, kCFBooleanFalse);
    return result;
  }
}
#pragma clang diagnostic pop

#if defined(ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT)
LuaSkinFoundationDecisionProbe probeLuaSkinFoundationResponseDecision(
    LuaSkinFoundationDecisionTest scenario) {
  @autoreleasepool {
    auto state = std::make_shared<FoundationHttpState>(LuaSkinHttpLimits{.maximumBytes = 1});
    auto *delegate = [[AsoLuaSkinHttpDelegate alloc] init];
    delegate->state = state;
    delegate->responseSemaphore = dispatch_semaphore_create(0);
    delegate->completionSemaphore = dispatch_semaphore_create(0);
    NSURLSession *session = [NSURLSession sessionWithConfiguration:[NSURLSessionConfiguration ephemeralSessionConfiguration]];
    NSURLSessionDataTask *task = [session dataTaskWithURL:[NSURL URLWithString:@"https://example.com"]];
    __block int allowed = 0;
    __block int cancelled = 0;
    if (scenario == LuaSkinFoundationDecisionTest::DisconnectBeforeResponse)
      resolveFoundationResponseDecision(*state, NSURLSessionResponseCancel);
    auto *response = [[NSHTTPURLResponse alloc] initWithURL:[NSURL URLWithString:@"https://example.com"]
        statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:@{@"Content-Length": @"12"}];
    [delegate URLSession:session dataTask:task didReceiveResponse:response
        completionHandler:^(NSURLSessionResponseDisposition disposition) {
          if (disposition == NSURLSessionResponseAllow) ++allowed;
          else if (disposition == NSURLSessionResponseCancel) ++cancelled;
        }];
    LuaSkinFoundationDecisionProbe result;
    result.beforeDecision = allowed + cancelled;
    if (scenario == LuaSkinFoundationDecisionTest::Read) {
      state->limits.maximumBytes = 12;
      resolveFoundationResponseDecision(*state, NSURLSessionResponseAllow);
    } else if (scenario == LuaSkinFoundationDecisionTest::CloseBeforeRead) {
      state->closed = true;
      resolveFoundationResponseDecision(*state, NSURLSessionResponseAllow);
    } else if (scenario == LuaSkinFoundationDecisionTest::Error) {
      [delegate URLSession:session task:task didCompleteWithError:
          [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]];
    } else { resolveFoundationResponseDecision(*state, NSURLSessionResponseCancel); }
    // Subsequent completion/disposal must not invoke an already consumed block.
    resolveFoundationResponseDecision(*state, NSURLSessionResponseCancel);
    [delegate URLSession:session task:task didCompleteWithError:nil];
    result.allowed = allowed;
    result.cancelled = cancelled;
    [task cancel];
    [session invalidateAndCancel];
    return result;
  }
}

std::unique_ptr<LuaSkinHttpConnection> createLuaSkinFoundationCandidateTestConnection(
    std::string url, LuaSkinHttpLimits limits, LuaSkinFoundationCandidateTestOptions options) {
  return std::make_unique<RestrictedFoundationHttpConnection>(std::move(url),
      std::move(limits), std::move(options));
}
#endif

} // namespace skin

#endif
