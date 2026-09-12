#import <Foundation/Foundation.h>

#include "RAII.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

void exerciseIOSMetadataTransport(const std::string &origin) {
  @autoreleasepool {
#if IOS_METADATA_HAS_BOUNDED_DELEGATE
    NSURLSession *session = [NSURLSession sessionWithConfiguration:
        NSURLSessionConfiguration.ephemeralSessionConfiguration];
    NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:origin.c_str()]];
    for (const bool known : {false, true}) {
      AsoTextDownloadDelegate *delegate = [[AsoTextDownloadDelegate alloc] init];
      delegate->maximumResponseBytes = 16;
      NSURLSessionDataTask *task = [session dataTaskWithURL:url];
      NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:url
          statusCode:200 HTTPVersion:@"HTTP/1.1"
          headerFields:known ? @{@"Content-Length": @"17"} : @{}];
      __block NSURLSessionResponseDisposition disposition = NSURLSessionResponseAllow;
      [delegate URLSession:session dataTask:task didReceiveResponse:response
          completionHandler:^(NSURLSessionResponseDisposition value) { disposition = value; }];
      expect((disposition == NSURLSessionResponseCancel) == known,
             "known oversized length rejected before accepting any response body");
      NSData *prefix = [@"12345678" dataUsingEncoding:NSUTF8StringEncoding];
      [delegate URLSession:session dataTask:task didReceiveData:prefix];
      [delegate URLSession:session dataTask:task didReceiveData:prefix];
      expect(delegate->responseBody.size() == (known ? 0 : 16),
             "retained bytes respect known admission and streamed exact boundary");
      [delegate URLSession:session dataTask:task didReceiveData:prefix];
      [delegate URLSession:session dataTask:task didReceiveData:prefix];
      expect(delegate->responseBody.size() == (known ? 0 : 16) && delegate->failureMessage != nil,
             "over-limit and late chunks never append or replace the size error");
      [task cancel];
    }
    AsoTextDownloadDelegate *aborted = [[AsoTextDownloadDelegate alloc] init];
    aborted->maximumResponseBytes = 16;
    aborted->abortRequested.store(true);
    NSURLSessionDataTask *task = [session dataTaskWithURL:url];
    [aborted URLSession:session dataTask:task didReceiveData:
        [@"late" dataUsingEncoding:NSUTF8StringEncoding]];
    expect(aborted->responseBody.empty(), "cancelled delegate cannot retain late data");
    [session invalidateAndCancel];
#endif
    for (const bool post : {false, true}) {
      const std::string method = post ? "POST" : "GET";
      for (const std::string route : {
               "/normal", "/exact", "/oversized", "/no-length", "/chunked",
               "/header-stall", "/stream-stall", "/redirect", "/redirect-over",
               "/invalid-redirect", "/utf8", "/invalid-utf8", "/nul", "/error",
               "/empty", "/cancel"}) {
        std::atomic_bool cancelled{false};
        std::thread canceller;
        if (route == "/cancel") {
          canceller = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            cancelled.store(true);
          });
        }
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        const auto body = (post ? postUrlText : fetchUrlText)(
            origin + route, error, &cancelled, 16);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (canceller.joinable()) canceller.join();
        const bool success = route == "/normal" || route == "/exact" ||
                             route == "/redirect" || route == "/utf8" ||
                             route == "/nul" || route == "/empty";
        const std::string label = method + route;
        expect(body.has_value() == success, label + " completion result");
        if (success && body) {
          const std::string expected = route == "/exact" ? "12345678abcdefgh" :
              route == "/utf8" ? "가나다" : route == "/nul" ? std::string("a\0b", 3) :
              route == "/empty" ? "" : method;
          expect(*body == expected, label + " complete UTF-8 bytes");
        }
        if (!success) expect(!error.empty(), label + " explicit error");
        if (route == "/error")
          expect(error.find("HTTP 503") != std::string::npos, label + " HTTP error preserved");
        if (route == "/oversized" || route == "/no-length" || route == "/chunked" ||
            route == "/header-stall" || route == "/stream-stall" || route == "/redirect-over")
          expect(error.find("limit") != std::string::npos, label + " size diagnostic");
        if (route == "/header-stall" || route == "/stream-stall" || route == "/cancel")
          expect(elapsed < std::chrono::seconds(1), label + " rejects before peer finishes");
        std::cout << label << " success=" << body.has_value()
                  << " elapsedMs="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " error=" << error << '\n';
      }
      std::string error;
      std::atomic_bool cancelled{false};
      expect(!(post ? postUrlText : fetchUrlText)(origin + "/normal", error, &cancelled, 0),
             method + " zero-byte cap rejects nonempty response");
      expect((post ? postUrlText : fetchUrlText)(origin + "/empty", error, &cancelled, 0) == "",
             method + " zero-byte cap permits empty response");
      cancelled.store(true);
      expect(!(post ? postUrlText : fetchUrlText)(origin + "/normal", error, &cancelled, 16),
             method + " pre-cancelled request fails");
      cancelled.store(false);
      expect(!(post ? postUrlText : fetchUrlText)("file:///fixture-must-not-open", error, &cancelled, 16),
             method + " non-web initial URL rejected");
    }
    std::string body;
    std::string error;
    expect(DownloadURLTextIOS(origin + "/normal", body, error, [] { return true; }) && body == "GET",
           "DifficultyTableImporter checkpoint signature remains callable");
    int checkpoints = 0;
    expect(DownloadURLTextIOS(origin + "/cancel", body, error, [&] {
      if (++checkpoints == 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      return true;
    }) && body == "12345678abcdefgh!" && checkpoints > 2,
           "DifficultyTableImporter pause checkpoint resumes the active transport");
    body = "previous body";
    checkpoints = 0;
    expect(!DownloadURLTextIOS(origin + "/normal", body, error,
                              [&] { return ++checkpoints == 1; }) &&
               body == "previous body",
           "completion checkpoint rejection never publishes a response");
#if IOS_METADATA_HAS_BOUNDED_DELEGATE
    AsoHttpsRedirectDelegate *delegate = [[AsoTextDownloadDelegate alloc] init];
#else
    AsoHttpsRedirectDelegate *delegate = [[AsoHttpsRedirectDelegate alloc] init];
#endif
    delegate->requireHttps = YES;
    __block BOOL followed = YES;
    [delegate URLSession:nil task:nil willPerformHTTPRedirection:nil
        newRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"http://fixture.invalid/target"]]
        completionHandler:^(NSURLRequest *request) { followed = request != nil; }];
    expect(!followed && delegate->rejectedInsecureRedirect,
           "HTTPS-origin downgrade rejected before follow");
  }
}
