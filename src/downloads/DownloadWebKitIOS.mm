#include "../targets.h"
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#include "DownloadWebKitIOS.h"
#include "BmsBrowser.h"
#include "BrowserPolicy.h"
#include "DownloadService.h"
#include "DownloadPlatform.h"
#include "DownloadIOSDispatch.h"
#include "../AtomicFile.h"
#include <algorithm>

@class AsoWebTransfer;
namespace {
NSString *string(const std::string &value) { return [NSString stringWithUTF8String:value.c_str()]; }
NSMutableDictionary<NSString *, AsoWebTransfer *> *transfers() {
  static NSMutableDictionary *value = [NSMutableDictionary new];
  return value; // Accessed only on the main queue.
}
constexpr NSUInteger maxResumeBytes = 4 * 1024 * 1024;
std::filesystem::path resumePath(const downloads::Job &job) {
  return downloads::service()->jobPath(job.id) / "webkit-resume.plist";
}
void saveResume(const downloads::Job &job, NSData *data, NSURL *file) {
  if (!data.length || data.length > maxResumeBytes || !file) return;
  auto owner = downloads::service(); const auto current = owner->get(job.id);
  if (!current || current->attempt != job.attempt || current->state == downloads::State::Cancelled) return;
  NSData *record = [NSPropertyListSerialization dataWithPropertyList:@{@"data":data, @"file":file.lastPathComponent}
      format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
  if (!record || record.length > maxResumeBytes) return;
  const auto operations = atomic_file::privateFileOperations(); std::string error;
  atomic_file::writeWithoutBackup(resumePath(job),
      {static_cast<const std::byte *>(record.bytes), record.length}, error, &operations);
}
NSData *readResume(const downloads::Job &job, NSURL **file) {
  const auto path = resumePath(job); std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || std::filesystem::file_size(path, error) > maxResumeBytes) return nil;
  NSData *bytes = [NSData dataWithContentsOfFile:string(path.string())];
  id record = bytes ? [NSPropertyListSerialization propertyListWithData:bytes options:NSPropertyListImmutable format:nil error:nil] : nil;
  std::filesystem::remove(path, error); // Never retry an invalid token indefinitely.
  if (![record isKindOfClass:NSDictionary.class] || ![record[@"data"] isKindOfClass:NSData.class] ||
      ![record[@"file"] isKindOfClass:NSString.class]) return nil;
  const std::string name = [record[@"file"] UTF8String];
  if (!name.starts_with("webkit.") || !name.ends_with(".part") || name.size() <= 12 ||
      !std::all_of(name.begin() + 7, name.end() - 5, [](unsigned char c) { return c >= '0' && c <= '9'; })) return nil;
  *file = [NSURL fileURLWithPath:string((downloads::service()->jobPath(job.id) / name).string())];
  return record[@"data"];
}
}

// Owns the browser engine and its transfer independently of the browser UI.
// OneDrive and other service-worker responses cannot be recreated by URLSession.
@interface AsoWebTransfer : NSObject <WKDownloadDelegate> {
@public
  downloads::Job job;
}
@property(nonatomic, copy) NSString *token;
@property(nonatomic, strong) WKWebView *web;
@property(nonatomic, strong) WKDownload *download;
@property(nonatomic, strong) NSURL *file;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, copy) void (^destination)(NSURL *);
@property(nonatomic, copy) void (^afterStop)(NSData *, NSURL *, WKWebView *);
@property(nonatomic) BOOL started;
@property(nonatomic) BOOL stopping;
@property(nonatomic) BOOL resuming;
- (void)start:(downloads::Job)snapshot resume:(NSData *)resume;
- (void)stop;
- (void)releaseTransfer;
@end

@implementation AsoWebTransfer
- (void)releaseTransfer {
  [self.timer invalidate]; self.timer = nil;
  self.download.delegate = nil; self.download = nil;
  if (transfers()[self.token] == self) [transfers() removeObjectForKey:self.token];
}
- (void)fail:(const char *)message {
  try { downloads::service()->transferFailed(job.id, job.attempt, message); } catch (...) {}
}
- (void)poll {
  if (self.stopping) return;
  try {
    auto owner = downloads::service();
    if (!owner->isCurrentTransfer(job.id, job.attempt)) { [self stop]; return; }
    NSProgress *progress = self.download.progress;
    const auto bytes = std::max<int64_t>(0, progress.completedUnitCount);
    const auto total = std::max<int64_t>(0, progress.totalUnitCount);
    NSError *error = nil;
    NSDictionary *space = [NSFileManager.defaultManager attributesOfFileSystemForPath:self.file.URLByDeletingLastPathComponent.path error:&error];
    const auto free = [space[NSFileSystemFreeSize] unsignedLongLongValue];
    const auto remaining = total > bytes ? static_cast<std::uint64_t>(total - bytes) : 0;
    constexpr std::uint64_t reserve = 512ull * 1024 * 1024;
    if (error || free < reserve || remaining > free - reserve) {
      [self fail:"Not enough storage for the download reserve."]; [self stop]; return;
    }
    owner->transferProgress(job.id, job.attempt, bytes, total);
  } catch (...) { [self fail:"Could not save browser download progress."]; [self stop]; }
}
- (void)start:(downloads::Job)snapshot resume:(NSData *)resume {
  job = std::move(snapshot); self.started = YES;
  if (!downloads::service()->isCurrentTransfer(job.id, job.attempt)) { [self stop]; return; }
  downloads::platform::beginImport(job); // The same continued-runtime lease covers transfer and import.
  __weak AsoWebTransfer *weakSelf = self;
  self.timer = [NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *) { [weakSelf poll]; }];
  [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
  if (self.destination) {
    self.file = [NSURL fileURLWithPath:string((downloads::service()->jobPath(job.id) /
        ("webkit." + std::to_string(job.attempt) + ".part")).string())];
    auto destination = self.destination; self.destination = nil;
    destination(self.file);
  } else if (resume.length) {
    self.resuming = YES;
    [self.web resumeDownloadFromResumeData:resume completionHandler:^(WKDownload *download) {
      self.resuming = NO; self.download = download; download.delegate = self;
      if (self.stopping || !downloads::service()->isCurrentTransfer(self->job.id, self->job.attempt)) {
        self.stopping = NO; [self stop]; return;
      }
    }];
  } else {
    [self fail:"This browser session cannot be resumed. Open Source and choose the download again."];
    [self releaseTransfer];
  }
}
- (void)stop {
  if (self.stopping) return;
  self.stopping = YES; [self.timer invalidate]; self.timer = nil;
  if (self.resuming) return; // Wait for the writer before collecting its resume checkpoint.
  const BOOL awaitingDestination = self.destination != nil;
  if (self.destination) { auto destination = self.destination; self.destination = nil; destination(nil); }
  void (^finished)(NSData *) = ^(NSData *data) {
    try { saveResume(self->job, data, self.file); } catch (...) {}
    auto next = self.afterStop; self.afterStop = nil;
    WKWebView *web = self.web; NSURL *file = self.file;
    [self releaseTransfer];
    if (next) next(data, file, web);
  };
  if (self.download && !awaitingDestination) [self.download cancel:finished]; else finished(nil);
}
- (void)download:(WKDownload *)download decideDestinationUsingResponse:(NSURLResponse *)response
    suggestedFilename:(NSString *)filename completionHandler:(void (^)(NSURL *))completion {
  // A resumed transfer may request a new destination; its previous file is kept
  // solely for WebKit's resume token until this callback supplies a new one.
  NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response : nil;
  if (self.stopping || !downloads::service()->isCurrentTransfer(job.id, job.attempt) || !http ||
      http.statusCode < 200 || http.statusCode >= 300 ||
      !downloads::allowedRedirect(job.request.url, response.URL.absoluteString.UTF8String ?: "")) {
    completion(nil); [self fail:"Browser download rejected an unsafe response."]; [self releaseTransfer]; return;
  }
  self.file = [NSURL fileURLWithPath:string((downloads::service()->jobPath(job.id) /
      ("webkit." + std::to_string(job.attempt) + ".part")).string())];
  completion(self.file);
}
- (void)download:(WKDownload *)download willPerformHTTPRedirection:(NSHTTPURLResponse *)response
    newRequest:(NSURLRequest *)request decisionHandler:(void (^)(WKDownloadRedirectPolicy))decision {
  // WKDownload cannot edit redirected headers. A cross-origin redirect needs a
  // fresh browser action rather than risking forwarding private request state.
  const bool safe = downloads::allowedRedirect(response.URL.absoluteString.UTF8String ?: "", request.URL.absoluteString.UTF8String ?: "") &&
      downloads::sameOrigin(response.URL.absoluteString.UTF8String ?: "", request.URL.absoluteString.UTF8String ?: "");
  decision(safe ? WKDownloadRedirectPolicyAllow : WKDownloadRedirectPolicyCancel);
  if (!safe) { [self fail:"Open Source and choose this redirected download again."]; [self stop]; }
}
- (void)download:(WKDownload *)download didFailWithError:(NSError *)error resumeData:(NSData *)resume {
  if (self.stopping) return;
  try { saveResume(job, resume, self.file); } catch (...) {}
  [self fail:resume.length ? "Browser download interrupted. Retry to continue." :
      "Browser download interrupted. Open Source and choose the download again."];
  [self releaseTransfer];
}
- (void)downloadDidFinish:(WKDownload *)download {
  if (self.stopping) return;
  [self.timer invalidate]; self.timer = nil;
  const auto snapshot = job; NSURL *file = self.file;
  // File protection and durable handoff can block; keep them off the UI queue.
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    try {
      NSError *error = nil;
      [NSFileManager.defaultManager setAttributes:@{NSFileProtectionKey:NSFileProtectionCompleteUntilFirstUserAuthentication}
          ofItemAtPath:file.path error:&error];
      if (error) throw std::runtime_error("File protection failed.");
      auto owner = downloads::service();
      if (owner->acceptFile(snapshot.id, snapshot.attempt, file.fileSystemRepresentation)) {
        std::error_code ignored; std::filesystem::remove(resumePath(snapshot), ignored);
      }
    } catch (...) {
      try { downloads::service()->transferFailed(snapshot.id, snapshot.attempt, "Could not retain the browser download. Free storage and retry."); } catch (...) {}
    }
    dispatch_async(dispatch_get_main_queue(), ^{ [self releaseTransfer]; });
  });
}
@end

namespace downloads::platform {
std::string admitWebKitDownload(WKDownload *download, WKWebView *web, NSURLResponse *response,
    NSString *filename, void (^destination)(NSURL *)) {
  NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response : nil;
  if (!http || http.statusCode < 200 || http.statusCode >= 300) {
    destination(nil); return "The website did not return a successful download response.";
  }
  if (transfers().count >= 32) { destination(nil); return "Finish or cancel a pending browser download first."; }
  auto request = browserDownloadRequest("GET", response.URL.absoluteString.UTF8String ?: "",
      (filename ?: @"download.archive").UTF8String, "", "", web.URL.absoluteString.UTF8String ?: "");
  if (!request) { destination(nil); return "Return to the app and reopen Browse Sites before downloading."; }
  NSString *token = [[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString];
  request->browserTransferId = token.UTF8String;
  AsoWebTransfer *transfer = [AsoWebTransfer new];
  transfer.token = token; transfer.web = web; transfer.download = download; transfer.destination = destination;
  transfers()[token] = transfer; download.delegate = transfer;
  try {
    transfer->job = service()->submit(std::move(*request));
    requestDownloadsPresentation(transfer->job.id);
    return {};
  } catch (...) {
    [transfer stop]; return "Could not save this download. Check free storage and try again.";
  }
}
std::string startWebKitTransfer(const Job &job) {
  dispatchOwnedJob(dispatch_get_main_queue(), job, ^(const Job &snapshot) {
    try {
      if (!service()->isCurrentTransfer(snapshot.id, snapshot.attempt)) return;
      NSString *token = string(snapshot.request.browserTransferId);
      AsoWebTransfer *existing = transfers()[token];
      if (existing && !existing.started && !existing.stopping) { [existing start:snapshot resume:nil]; return; }
      // A retry must wait for the previous writer and cancellation checkpoint.
      const Job next = snapshot;
      void (^start)(NSData *, NSURL *, WKWebView *) = ^(NSData *data, NSURL *file, WKWebView *web) {
        if (!service()->isCurrentTransfer(next.id, next.attempt)) return;
        AsoWebTransfer *transfer = [AsoWebTransfer new]; transfer.token = token; transfer.file = file;
        if (!web) {
          WKWebViewConfiguration *config = [WKWebViewConfiguration new];
          config.websiteDataStore = WKWebsiteDataStore.nonPersistentDataStore;
          web = [[WKWebView alloc] initWithFrame:CGRectZero configuration:config];
        }
        transfer.web = web; transfers()[token] = transfer;
        [transfer start:next resume:data];
      };
      if (existing) { existing.afterStop = start; [existing stop]; }
      else { NSURL *file = nil; NSData *resume = readResume(snapshot, &file); start(resume, file, nil); }
    } catch (...) {
      try { service()->transferFailed(snapshot.id, snapshot.attempt, "Could not resume this browser download. Open Source and choose it again."); } catch (...) {}
    }
  });
  return {};
}
void cancelWebKitTransfer(const Job &job) {
  dispatchOwnedJob(dispatch_get_main_queue(), job, ^(const Job &snapshot) {
    AsoWebTransfer *transfer = transfers()[string(snapshot.request.browserTransferId)];
    if (transfer && transfer->job.id == snapshot.id && (!transfer.started || transfer->job.attempt == snapshot.attempt)) [transfer stop];
  });
}
}
#endif
