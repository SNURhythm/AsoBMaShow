#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include "RAII.h"
#include "BmsSearchService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

enum class NativeFileFault {
  None,
  Copy,
  Read,
  Write,
  Close,
  StageRename,
  PublishRename,
  GrowStagedFile,
  CancelCopy,
  CancelBeforePublish
};

struct NativeFileIoFixture {
  NativeFileFault fault = NativeFileFault::None;
  std::atomic_size_t maximumRead{0};
  std::atomic_size_t readCalls{0};
  std::atomic_size_t writeCalls{0};
  std::atomic_size_t writtenBytes{0};
  std::atomic_int outputDescriptor{-1};
  std::atomic_bool copyObservedAbort{false};
  std::atomic_bool *cancelled = nullptr;
  std::atomic_bool *abortRequested = nullptr;
};

NativeFileIoFixture nativeIo;

int fixtureRename(const char *source, const char *destination) {
  const bool staging = std::filesystem::path(destination).filename() == "archive";
  if (staging && nativeIo.fault == NativeFileFault::StageRename) {
    errno = EACCES;
    return -1;
  }
  if (staging && (nativeIo.fault == NativeFileFault::Copy ||
                  nativeIo.fault == NativeFileFault::Read ||
                  nativeIo.fault == NativeFileFault::Write ||
                  nativeIo.fault == NativeFileFault::Close ||
                  nativeIo.fault == NativeFileFault::CancelCopy)) {
    errno = EXDEV;
    return -1;
  }
  if (!staging && nativeIo.fault == NativeFileFault::PublishRename) {
    errno = EACCES;
    return -1;
  }
  return ::rename(source, destination);
}

ssize_t fixtureRead(int descriptor, void *buffer, size_t bytes) {
  nativeIo.maximumRead.store(std::max(nativeIo.maximumRead.load(), bytes));
  const size_t invocation = nativeIo.readCalls.fetch_add(1);
  if (nativeIo.fault == NativeFileFault::Read && invocation > 0) {
    errno = EIO;
    return -1;
  }
  const ssize_t count = ::read(descriptor, buffer, bytes);
  if (nativeIo.fault == NativeFileFault::CancelCopy && count > 0) {
    nativeIo.cancelled->store(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!nativeIo.abortRequested->load() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    nativeIo.copyObservedAbort.store(nativeIo.abortRequested->load());
  }
  return count;
}

ssize_t fixtureWrite(int descriptor, const void *buffer, size_t bytes) {
  nativeIo.outputDescriptor.store(descriptor);
  const size_t invocation = nativeIo.writeCalls.fetch_add(1);
  if (nativeIo.fault == NativeFileFault::Write && invocation > 0) {
    errno = EIO;
    return -1;
  }
  const size_t requested = nativeIo.fault == NativeFileFault::Write
                               ? std::min<size_t>(4096, bytes)
                               : bytes;
  const ssize_t written = ::write(descriptor, buffer, requested);
  if (written > 0) nativeIo.writtenBytes.fetch_add(static_cast<size_t>(written));
  return written;
}

int fixtureClose(int descriptor) {
  const int result = ::close(descriptor);
  if (nativeIo.fault == NativeFileFault::Close &&
      descriptor == nativeIo.outputDescriptor.load()) {
    nativeIo.outputDescriptor.store(-1);
    errno = EIO;
    return -1;
  }
  return result;
}

int fixtureLstat(const char *path, struct stat *status) {
  if (nativeIo.fault == NativeFileFault::GrowStagedFile &&
      ::truncate(path, 65537) != 0)
    return -1;
  const int result = ::lstat(path, status);
  if (nativeIo.fault == NativeFileFault::CancelBeforePublish)
    nativeIo.cancelled->store(true);
  return result;
}

#define rename fixtureRename
#define read fixtureRead
#define write fixtureWrite
#define close fixtureClose
#define lstat fixtureLstat
#include "transport_native_methods.inc"
#undef rename
#undef read
#undef write
#undef close
#undef lstat

#if TRANSPORT_HAS_FILE_BRIDGE
AsoFileDownloadDelegate *observedFileDelegate = nil;
std::filesystem::path observedStagingDirectory;
IMP originalSessionFactory = nullptr;

NSURLSession *recordSession(id receiver, SEL selector,
                            NSURLSessionConfiguration *configuration,
                            id<NSURLSessionDelegate> delegate,
                            NSOperationQueue *queue) {
  if ([delegate isKindOfClass:[AsoFileDownloadDelegate class]]) {
    observedFileDelegate = (AsoFileDownloadDelegate *)delegate;
    observedStagingDirectory = observedFileDelegate->stagingDirectory;
    nativeIo.abortRequested = &observedFileDelegate->abortRequested;
  }
  using Factory = NSURLSession *(*)(id, SEL, NSURLSessionConfiguration *,
                                    id<NSURLSessionDelegate>, NSOperationQueue *);
  return reinterpret_cast<Factory>(originalSessionFactory)(receiver, selector,
                                                           configuration, delegate, queue);
}

@interface TransportResponseTaskFixture : NSObject
@property(nonatomic, strong) NSURLResponse *response;
- (void)cancel;
@end

@implementation TransportResponseTaskFixture
- (void)cancel {}
@end
#endif

std::atomic_size_t wholeFileReads{0};
std::atomic_bool recordedFileReadStack{false};
IMP originalFileRead = nullptr;
id recordWholeFileRead(id receiver, SEL selector, NSURL *url,
                        NSDataReadingOptions options, NSError **error) {
  wholeFileReads.fetch_add(1);
  if (!recordedFileReadStack.exchange(true))
    std::cerr << "First whole-file NSData read stack: "
              << NSThread.callStackSymbols.description.UTF8String << '\n';
  using Method = id (*)(id, SEL, NSURL *, NSDataReadingOptions, NSError **);
  return reinterpret_cast<Method>(originalFileRead)(receiver, selector, url,
                                                     options, error);
}

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void cancelOnProgress(void *context, std::uint64_t bytes, std::uint64_t) {
  if (bytes >= 4096) static_cast<std::atomic_bool *>(context)->store(true);
}

bool download(const std::string &url, const std::filesystem::path &path,
              std::atomic_bool &cancelled, std::string &error,
              bool cancelDuring = false) {
#if TRANSPORT_HAS_FILE_BRIDGE
  return DownloadURLToFileIOS(url, path, cancelled,
                              cancelDuring ? 128 * 1024 : 64 * 1024, error,
                              cancelDuring ? cancelOnProgress : nullptr,
                              &cancelled);
#else
  return downloadUrlToFile(url, path, cancelled, error,
      [&](const BmsSearchDownloadProgress &progress) {
        if (cancelDuring)
          cancelOnProgress(&cancelled, progress.downloadedBytes,
                            progress.totalBytes);
      });
#endif
}

bool expectedFile(const std::filesystem::path &path, char value, size_t size) {
  std::ifstream input(path, std::ios::binary);
  std::array<char, 4096> buffer;
  size_t actual = 0;
  while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
    for (std::streamsize index = 0; index < input.gcount(); ++index)
      if (buffer[index] != value) return false;
    actual += static_cast<size_t>(input.gcount());
  }
  return input.eof() && actual == size;
}

#if TRANSPORT_HAS_FILE_BRIDGE
void exerciseFileFaults(const std::string &origin,
                         const std::filesystem::path &root) {
  Method factory = class_getClassMethod(NSURLSession.class,
      @selector(sessionWithConfiguration:delegate:delegateQueue:));
  originalSessionFactory = method_setImplementation(factory,
      reinterpret_cast<IMP>(recordSession));
  auto restoreFactory = makeScopeExit([&] {
    method_setImplementation(factory, originalSessionFactory);
    observedFileDelegate = nil;
    nativeIo.fault = NativeFileFault::None;
    nativeIo.cancelled = nullptr;
    nativeIo.abortRequested = nullptr;
  });
  const auto destination = root / "archive.zip";
  const auto sentinel = root / "unrelated";
  for (const auto fault : {NativeFileFault::Copy, NativeFileFault::Read,
                          NativeFileFault::Write, NativeFileFault::Close,
                          NativeFileFault::StageRename, NativeFileFault::PublishRename,
                          NativeFileFault::GrowStagedFile, NativeFileFault::CancelCopy,
                          NativeFileFault::CancelBeforePublish}) {
    observedFileDelegate = nil;
    nativeIo.fault = fault;
    nativeIo.maximumRead.store(0);
    nativeIo.readCalls.store(0);
    nativeIo.writeCalls.store(0);
    nativeIo.writtenBytes.store(0);
    nativeIo.outputDescriptor.store(-1);
    nativeIo.copyObservedAbort.store(false);
    std::atomic_bool cancelled{false};
    nativeIo.cancelled = &cancelled;
    std::ofstream(destination, std::ios::trunc) << "zzz";
    std::string error;
    const bool success = download(origin + "/exact", destination, cancelled, error);
    const std::string label = "native fault " + std::to_string(static_cast<int>(fault));
    expect(success == (fault == NativeFileFault::Copy), label + " result");
    expect(expectedFile(destination, fault == NativeFileFault::Copy ? 'a' : 'z',
                        fault == NativeFileFault::Copy ? 65536 : 3),
           label + " failed publication preserves previous destination");
    expect(expectedFile(sentinel, 'z', 1), label + " unrelated sentinel preserved");
    expect(nativeIo.maximumRead.load() <= 65536, label + " fixed copy read bound");
    if (fault == NativeFileFault::Copy)
      expect(nativeIo.maximumRead.load() == 65536 && nativeIo.writtenBytes.load() == 65536,
             label + " actual EXDEV fallback copies full file in bounded IO");
    if (fault == NativeFileFault::Read || fault == NativeFileFault::Write ||
        fault == NativeFileFault::Close)
      expect(nativeIo.writtenBytes.load() > 0 && !error.empty(),
             label + " failure after partial IO does not publish");
    if (fault == NativeFileFault::CancelCopy)
      expect(nativeIo.copyObservedAbort.load() && nativeIo.writtenBytes.load() == 0,
             label + " waiter abort reaches active copy before write");
    if (fault == NativeFileFault::CancelCopy || fault == NativeFileFault::CancelBeforePublish)
      expect(error.find("cancel") != std::string::npos, label + " cancellation diagnostic");
    if (fault == NativeFileFault::GrowStagedFile)
      expect(error.find("limit") != std::string::npos, label + " final size admission");
    expect(observedFileDelegate != nil, label + " actual session delegate observed");
    if (fault == NativeFileFault::CancelCopy && observedFileDelegate != nil) {
      const long completion = dispatch_semaphore_wait(observedFileDelegate->semaphore,
          dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
      expect(completion == 0, label + " cancelled task completes before late-callback probes");
      if (completion != 0) continue;
    }
    if (observedFileDelegate != nil) {
      expect(observedFileDelegate->abortRequested.load() &&
                 observedFileDelegate->progressCallback == nullptr &&
                 observedFileDelegate->progressContext == nullptr,
             label + " borrowed callback context detached before return");
      expect(!std::filesystem::exists(observedStagingDirectory), label + " stage cleanup");
      NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc]
          initWithURL:[NSURL URLWithString:@"http://127.0.0.1/late"]
          statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:@{}];
      TransportResponseTaskFixture *lateTask = [[TransportResponseTaskFixture alloc] init];
      lateTask.response = response;
      expect(![observedFileDelegate admitResponse:response task:(NSURLSessionTask *)lateTask],
             label + " late callback is cancelled even with valid HTTP response");
      [observedFileDelegate URLSession:nil downloadTask:(NSURLSessionDownloadTask *)lateTask
          didWriteData:1 totalBytesWritten:1 totalBytesExpectedToWrite:1];
      [observedFileDelegate URLSession:nil downloadTask:(NSURLSessionDownloadTask *)lateTask
          didFinishDownloadingToURL:[NSURL fileURLWithPath:
              [NSString stringWithUTF8String:sentinel.c_str()]]];
      expect(expectedFile(sentinel, 'z', 1) &&
                 !std::filesystem::exists(observedStagingDirectory),
             label + " late finish cannot consume source or recreate owned stage");
      std::filesystem::create_directory(observedStagingDirectory);
      std::ofstream(observedStagingDirectory / "later-owner") << "z";
      [observedFileDelegate cleanupStaging];
      expect(expectedFile(observedStagingDirectory / "later-owner", 'z', 1),
             label + " repeat cleanup does not remove a later same-name directory");
      std::filesystem::remove_all(observedStagingDirectory);
    }
    std::cout << label << " success=" << success
              << " maxRead=" << nativeIo.maximumRead.load()
              << " writtenBytes=" << nativeIo.writtenBytes.load()
              << " error=" << error << '\n';
  }
  nativeIo.fault = NativeFileFault::None;
  std::atomic_bool cancelled{true};
  std::string error;
  expect(!download(origin + "/normal", destination, cancelled, error) &&
             error.find("cancel") != std::string::npos,
         "pre-cancelled native request is rejected");
  cancelled.store(false);
  std::atomic_size_t progressCalls{0};
  const auto throwingProgress = [](void *context, std::uint64_t, std::uint64_t) {
    static_cast<std::atomic_size_t *>(context)->fetch_add(1);
    throw std::runtime_error("fixture progress failure");
  };
  std::ofstream(destination, std::ios::trunc) << "zzz";
  expect(!DownloadURLToFileIOS(origin + "/exact", destination, cancelled, 65536,
                               error, throwingProgress, &progressCalls),
         "native callback exception fails without crossing Foundation boundary");
  expect(progressCalls.load() > 0 && expectedFile(destination, 'z', 3),
         "throwing real progress callback does not replace destination");
  AsoFileDownloadDelegate *httpsDelegate = [[AsoFileDownloadDelegate alloc] init];
  httpsDelegate->requireHttps = YES;
  NSHTTPURLResponse *redirect = [[NSHTTPURLResponse alloc]
      initWithURL:[NSURL URLWithString:@"https://fixture.invalid/source"]
      statusCode:302 HTTPVersion:@"HTTP/1.1" headerFields:@{}];
  __block BOOL followed = YES;
  [httpsDelegate URLSession:nil task:nil willPerformHTTPRedirection:redirect
      newRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"http://fixture.invalid/target"]]
      completionHandler:^(NSURLRequest *request) { followed = request != nil; }];
  expect(!followed && httpsDelegate->rejectedInsecureRedirect,
         "actual file delegate rejects initial HTTPS downgrade before following");
  AsoFileDownloadDelegate *httpDelegate = [[AsoFileDownloadDelegate alloc] init];
  [httpDelegate URLSession:nil task:nil willPerformHTTPRedirection:redirect
      newRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"https://fixture.invalid/target"]]
      completionHandler:^(NSURLRequest *request) { followed = request != nil; }];
  expect(followed, "actual file delegate preserves allowed HTTP-origin upgrade");
  [httpDelegate URLSession:nil task:nil willPerformHTTPRedirection:redirect
      newRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"file:///fixture-not-opened"]]
      completionHandler:^(NSURLRequest *request) { followed = request != nil; }];
  expect(!followed && httpDelegate->rejectedInvalidRedirect,
         "actual file delegate rejects non-web redirect before dispatch");
}
#endif

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  @autoreleasepool {
    const std::string origin = argv[1];
    if (!origin.starts_with("http://127.0.0.1:")) return 2;
    const auto root = std::filesystem::temp_directory_path() /
        ("asobmshow-transport-" + std::string(NSUUID.UUID.UUIDString.UTF8String));
    std::filesystem::create_directory(root);
    const auto destination = root / "archive.zip";
    const auto sentinel = root / "unrelated";
    std::ofstream(sentinel) << "z";
    Method method = class_getClassMethod(NSData.class,
        @selector(dataWithContentsOfURL:options:error:));
    originalFileRead = method_setImplementation(method,
        reinterpret_cast<IMP>(recordWholeFileRead));
    for (const std::string route : {"/normal", "/redirect", "/replace", "/exact",
                                    "/oversized", "/no-length", "/chunked",
                                    "/error", "/cancel", "/stall",
                                    "/invalid-redirect", "/truncated"}) {
      std::filesystem::remove(destination);
      if (route == "/replace") std::ofstream(destination) << "old";
      std::atomic_bool cancelled{false};
      std::string error;
      wholeFileReads.store(0);
      std::thread canceller;
      if (route == "/stall") {
        canceller = std::thread([&] {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          cancelled.store(true);
        });
      }
      const auto start = std::chrono::steady_clock::now();
      const bool success = download(origin + route, destination, cancelled,
                                     error, route == "/cancel");
      const auto elapsed = std::chrono::steady_clock::now() - start;
      if (canceller.joinable()) canceller.join();
      std::error_code sizeError;
      const auto bytes = std::filesystem::file_size(destination, sizeError);
      std::cout << route << " success=" << success
                << " publishedBytes=" << (sizeError ? 0 : bytes)
                << " wholeFileReads=" << wholeFileReads.load()
                << " elapsedMs="
                << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                << " error=" << error << '\n';
      const bool normal = route == "/normal" || route == "/redirect" ||
                          route == "/replace" || route == "/exact";
      expect(success == normal, route + " completion result");
      expect(wholeFileReads.load() == 0,
             route + " actual delegate never materializes complete NSData");
      if (normal) {
        expect(expectedFile(destination, 'a', route == "/exact" ? 65536 : 16384),
               route + " exact file bytes");
      } else {
        expect(!std::filesystem::exists(destination), route + " no publication");
        expect(!error.empty(), route + " explicit failure");
      }
      if (route == "/stall")
        expect(elapsed < std::chrono::seconds(1), "idle task promptly cancels");
      if (route == "/stall" || route == "/cancel")
        expect(error.find("cancel") != std::string::npos,
               route + " cancellation, not a size/status rejection");
      expect(expectedFile(sentinel, 'z', 1), route + " unrelated file preserved");
      size_t entries = 0;
      for (const auto &entry : std::filesystem::directory_iterator(root)) {
        (void)entry;
        ++entries;
      }
      expect(entries == (normal ? 2 : 1), route + " owned sibling cleanup");
    }
    std::ofstream(destination, std::ios::trunc) << "zzz";
    std::atomic_bool cancelled{false};
    std::string error;
    expect(!download(origin + "/error", destination, cancelled, error),
           "failed replacement does not report success");
    expect(expectedFile(destination, 'z', 3),
           "failed replacement preserves original destination");
    wholeFileReads.store(0);
    expect(downloadUrlToFile(origin + "/normal", destination, cancelled, error, {}),
           "actual Find BMS archive caller completes");
    expect(expectedFile(destination, 'a', 16384) && wholeFileReads.load() == 0,
           "actual Find BMS archive caller uses the file bridge");
#if TRANSPORT_HAS_FILE_BRIDGE
    exerciseFileFaults(origin, root);
#endif
    method_setImplementation(method, originalFileRead);
    std::filesystem::remove_all(root);
  }
  std::cout << "macOS Foundation native bridge failures=" << failures
            << "; NOT iOS runtime validation\n";
  return failures == 0 ? 0 : 1;
}
