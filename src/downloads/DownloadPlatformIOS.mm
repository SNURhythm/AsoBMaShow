#include "../targets.h"
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <BackgroundTasks/BackgroundTasks.h>
#import <objc/runtime.h>
#include "DownloadPlatform.h"
#include "DownloadService.h"
#include "DownloadIOSDispatch.h"
#include "BrowserPolicy.h"
#include "../AtomicFile.h"
#include <charconv>
#include <algorithm>
#include <map>

namespace {
NSString *const sessionIdentifier = @"com.snurhythm.AsoBMaShow.downloads.v1";
using Identity = std::pair<std::string, std::uint64_t>;
std::optional<Identity> identity(NSString *description) {
  if (!description) return {};
  const std::string text = description.UTF8String;
  if (text.size() < 34 || text[32] != ':' ||
      !std::all_of(text.begin(), text.begin() + 32, [](unsigned char c) { return std::isxdigit(c); })) return {};
  std::uint64_t attempt = 0;
  const auto parsed = std::from_chars(text.data() + 33, text.data() + text.size(), attempt);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
  return Identity{text.substr(0, 32), attempt};
}
NSString *descriptor(const downloads::Job &job) {
  return [NSString stringWithFormat:@"%s:%llu", job.id.c_str(), static_cast<unsigned long long>(job.attempt)];
}
NSString *continuedIdentifier(NSString *description) {
  return [NSString stringWithFormat:@"%@.downloads.%@", NSBundle.mainBundle.bundleIdentifier,
      [description stringByReplacingOccurrencesOfString:@":" withString:@"."]];
}
bool persistentTransfer(const downloads::Request &request) {
  // Background URLSession does not expose redirect decisions. The user chose
  // legacy HTTP compatibility, so requests requiring HTTPS or scoped headers
  // keep delegate-controlled redirects under a continued-processing lease.
  return downloads::httpOrigin(request.url).value_or("").starts_with("http:") &&
      std::none_of(request.headers.begin(), request.headers.end(), [](const auto &header) {
        return header.first == "Cookie" || header.first == "Referer";
      });
}
void saveResume(const Identity &key, NSData *data) {
  if (!data || data.length > 4 * 1024 * 1024) return;
  auto owner = downloads::service();
  const auto job = owner->get(key.first);
  if (!job || job->attempt != key.second || job->state == downloads::State::Cancelled) return;
  const auto path = owner->jobPath(key.first) / "resume.data";
  std::string error;
  const auto operations = atomic_file::privateFileOperations();
  atomic_file::writeWithoutBackup(path,
      {static_cast<const std::byte *>(data.bytes), data.length}, error, &operations);
}
void failTransfer(const Identity &key, const char *message) {
  downloads::service()->transferFailed(key.first, key.second, message);
}
}

@interface AsoDownloadHost : NSObject <NSURLSessionDownloadDelegate>
@property(nonatomic, strong) NSURLSession *background;
@property(nonatomic, strong) NSURLSession *foreground;
@property(nonatomic, copy) void (^backgroundCompletion)(void);
@property(nonatomic, strong) NSMutableDictionary<NSString *, BGTask *> *continued;
@property(nonatomic, strong) NSMutableSet<NSString *> *registered;
@property(nonatomic, strong) NSMutableDictionary<NSString *, NSNumber *> *runtimeRevisions;
- (void)reconcile;
- (void)requestRuntime:(downloads::Job)job;
- (void)finishRuntime:(NSString *)description success:(BOOL)success;
- (void)stop:(NSString *)description;
@end

namespace {
AsoDownloadHost *host() {
  static AsoDownloadHost *value;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ value = [AsoDownloadHost new]; });
  return value;
}
}
@implementation AsoDownloadHost
- (instancetype)init {
  self = [super init];
  if (!self) return nil;
  _continued = [NSMutableDictionary new]; _registered = [NSMutableSet new];
  _runtimeRevisions = [NSMutableDictionary new];
  NSOperationQueue *queue = [NSOperationQueue new]; queue.maxConcurrentOperationCount = 1;
  NSURLSessionConfiguration *background = [NSURLSessionConfiguration backgroundSessionConfigurationWithIdentifier:sessionIdentifier];
  background.sessionSendsLaunchEvents = YES; background.discretionary = NO;
  background.HTTPShouldSetCookies = NO; background.HTTPCookieStorage = nil;
  background.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  background.timeoutIntervalForRequest = 90;
  _background = [NSURLSession sessionWithConfiguration:background delegate:self delegateQueue:queue];
  NSURLSessionConfiguration *foreground = NSURLSessionConfiguration.ephemeralSessionConfiguration;
  foreground.HTTPShouldSetCookies = NO; foreground.HTTPCookieStorage = nil;
  foreground.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  foreground.waitsForConnectivity = NO; foreground.timeoutIntervalForRequest = 90;
  _foreground = [NSURLSession sessionWithConfiguration:foreground delegate:self delegateQueue:queue];
  return self;
}
- (void)reconcile {
  const auto savedJobs = downloads::service()->jobs();
  [self.background getAllTasksWithCompletionHandler:^(NSArray<__kindof NSURLSessionTask *> *backgroundTasks) {
    [self.foreground getAllTasksWithCompletionHandler:^(NSArray<__kindof NSURLSessionTask *> *foregroundTasks) {
    try {
      NSArray *tasks = [backgroundTasks arrayByAddingObjectsFromArray:foregroundTasks];
      auto owner = downloads::service(); std::set<Identity> alive;
      for (NSURLSessionTask *task in tasks) {
        const auto key = identity(task.taskDescription);
        if (!key || !owner->isCurrentTransfer(key->first, key->second)) { [task cancel]; continue; }
        alive.insert(*key);
        [task resume];
      }
      for (const auto &job : savedJobs) {
        if (job.state == downloads::State::Transferring && !alive.contains({job.id, job.attempt}))
          owner->transferFailed(job.id, job.attempt, "Download interrupted. Retry to continue while the app is open.");
      }
    } catch (...) { /* Preserve the manifest if protected data is unavailable. */ }
    }];
  }];
}
- (void)requestRuntime:(downloads::Job)job {
  if (@available(iOS 26.0, *)) {
    if (UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    NSString *description = descriptor(job);
    if (self.runtimeRevisions[description]) return; // Keep the transfer's existing lease through import.
    const auto current = downloads::service()->get(job.id);
    if (!current || current->attempt != job.attempt || downloads::terminal(current->state) ||
        current->state == downloads::State::NeedsAttention) return;
    NSNumber *revision = @(job.revision);
    NSString *identifier = [continuedIdentifier(description) stringByAppendingFormat:@".%llu", job.revision];
    self.runtimeRevisions[description] = revision;
    if (![self.registered containsObject:identifier]) {
      BOOL registered = [BGTaskScheduler.sharedScheduler registerForTaskWithIdentifier:identifier
          usingQueue:dispatch_get_main_queue() launchHandler:^(__kindof BGTask *task) {
        try {
          auto owner = downloads::service(); const auto current = owner->get(job.id);
          if (![self.runtimeRevisions[description] isEqualToNumber:revision] ||
              !current || current->attempt != job.attempt || downloads::terminal(current->state) ||
              current->state == downloads::State::NeedsAttention) {
            [task setTaskCompletedWithSuccess:NO]; return;
          }
          self.continued[description] = task;
          task.expirationHandler = ^{
            dispatch_async(dispatch_get_main_queue(), ^{
              if (![self.runtimeRevisions[description] isEqualToNumber:revision]) return;
              try {
                const auto current = downloads::service()->get(job.id);
                if (current && current->attempt == job.attempt)
                  downloads::service()->interrupt(job.id, "Background runtime ended. Retry to continue.");
              } catch (...) {}
              [self stop:description];
              [self finishRuntime:description success:NO];
            });
          };
          owner->setContinuedProcessing(job.id, true);
          if ([task isKindOfClass:BGContinuedProcessingTask.class]) {
            auto continued = (BGContinuedProcessingTask *)task;
            continued.progress.totalUnitCount = 1000;
          }
        } catch (...) { [task setTaskCompletedWithSuccess:NO]; }
      }];
      if (!registered) { [self.runtimeRevisions removeObjectForKey:description]; return; }
      [self.registered addObject:identifier];
    }
    BGContinuedProcessingTaskRequest *request = [[BGContinuedProcessingTaskRequest alloc]
        initWithIdentifier:identifier title:@"BMS download" subtitle:@"Downloading and importing"];
    request.strategy = BGContinuedProcessingTaskRequestSubmissionStrategyFail;
    request.requiredResources = BGContinuedProcessingTaskRequestResourcesDefault;
    NSError *error = nil;
    if (![BGTaskScheduler.sharedScheduler submitTaskRequest:request error:&error])
      [self.runtimeRevisions removeObjectForKey:description];
    // Admission failure leaves the explicit foreground fallback available.
  }
}
- (void)finishRuntime:(NSString *)description success:(BOOL)success {
  if (@available(iOS 26.0, *)) {
    NSNumber *revision = self.runtimeRevisions[description];
    if (revision) [BGTaskScheduler.sharedScheduler cancelTaskRequestWithIdentifier:
        [continuedIdentifier(description) stringByAppendingFormat:@".%llu", revision.unsignedLongLongValue]];
    [self.runtimeRevisions removeObjectForKey:description];
    BGTask *task = self.continued[description];
    [self.continued removeObjectForKey:description];
    if (success && [task isKindOfClass:BGContinuedProcessingTask.class]) {
      ((BGContinuedProcessingTask *)task).progress.completedUnitCount = 1000;
    }
    [task setTaskCompletedWithSuccess:success];
  }
  const auto key = identity(description);
  if (key) {
    try {
      const auto current = downloads::service()->get(key->first);
      if (current && current->attempt == key->second)
        downloads::service()->setContinuedProcessing(key->first, false);
    } catch (...) {}
  }
}
- (void)stop:(NSString *)description {
  dispatch_async(dispatch_get_main_queue(), ^{
    // Save Foundation's resume token before ordinary suspension can end the
    // delegate callback. The durable Failed/Cancelled state is already saved.
    __block UIBackgroundTaskIdentifier cleanup = UIBackgroundTaskInvalid;
    cleanup = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Save download progress" expirationHandler:^{
      if (cleanup != UIBackgroundTaskInvalid) {
        [UIApplication.sharedApplication endBackgroundTask:cleanup]; cleanup = UIBackgroundTaskInvalid;
      }
    }];
    dispatch_group_t pending = dispatch_group_create();
    for (NSURLSession *session in @[self.background, self.foreground]) {
      dispatch_group_enter(pending);
      [session getAllTasksWithCompletionHandler:^(NSArray<__kindof NSURLSessionTask *> *tasks) {
        for (NSURLSessionTask *task in tasks) {
          if (![task.taskDescription isEqualToString:description]) continue;
          const auto key = identity(description);
          if ([task isKindOfClass:NSURLSessionDownloadTask.class] && key) {
            dispatch_group_enter(pending);
            [(NSURLSessionDownloadTask *)task cancelByProducingResumeData:^(NSData *data) {
              try { saveResume(*key, data); } catch (...) {}
              dispatch_group_leave(pending);
            }];
          } else [task cancel];
        }
        dispatch_group_leave(pending);
      }];
    }
    dispatch_group_notify(pending, dispatch_get_main_queue(), ^{
      if (cleanup != UIBackgroundTaskInvalid) {
        [UIApplication.sharedApplication endBackgroundTask:cleanup]; cleanup = UIBackgroundTaskInvalid;
      }
    });
  });
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request
    completionHandler:(void (^)(NSURLRequest *))completion {
  try {
    const auto key = identity(task.taskDescription);
    auto owner = downloads::service(); const auto job = key ? owner->get(key->first) : std::nullopt;
    if (!job || !owner->isCurrentTransfer(key->first, key->second) ||
        !downloads::allowedRedirect(response.URL.absoluteString.UTF8String, request.URL.absoluteString.UTF8String)) {
      completion(nil); [task cancel];
      if (key) failTransfer(*key, "Download rejected an unsafe redirect.");
      return;
    }
    NSMutableURLRequest *next = [request mutableCopy];
    if (!downloads::sameOrigin(response.URL.absoluteString.UTF8String, request.URL.absoluteString.UTF8String)) {
      [next setValue:nil forHTTPHeaderField:@"Cookie"];
      [next setValue:nil forHTTPHeaderField:@"Referer"];
      [next setValue:nil forHTTPHeaderField:@"Authorization"];
    }
    completion(next);
  } catch (...) { completion(nil); [task cancel]; }
}
- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
    didWriteData:(int64_t)written totalBytesWritten:(int64_t)bytes totalBytesExpectedToWrite:(int64_t)total {
  try {
    const auto key = identity(task.taskDescription);
    auto owner = downloads::service();
    if (!key || !owner->isCurrentTransfer(key->first, key->second)) { [task cancel]; return; }
    // Preserve the existing 512 MiB reserve without allocating archive-sized buffers.
    NSError *error = nil;
    NSDictionary *space = [NSFileManager.defaultManager attributesOfFileSystemForPath:NSTemporaryDirectory() error:&error];
    const auto free = [space[NSFileSystemFreeSize] unsignedLongLongValue];
    const auto remaining = total > bytes ? static_cast<std::uint64_t>(total - bytes) : 0;
    constexpr std::uint64_t reserve = 512ull * 1024 * 1024;
    if (error || free < reserve || remaining > free - reserve) {
      [self stop:task.taskDescription]; failTransfer(*key, "Not enough storage for the download reserve."); return;
    }
    owner->transferProgress(key->first, key->second, std::max<std::int64_t>(0, bytes), std::max<std::int64_t>(0, total));
  } catch (...) { [task cancel]; }
}
- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)task
    didFinishDownloadingToURL:(NSURL *)location {
  try {
    const auto key = identity(task.taskDescription);
    auto owner = downloads::service();
    if (!key || !owner->isCurrentTransfer(key->first, key->second)) return;
    auto response = [task.response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)task.response : nil;
    const auto job = owner->get(key->first);
    if (!response || response.statusCode < 200 || response.statusCode >= 300 ||
        !downloads::allowedRedirect(downloads::transferRequest(*job).url, response.URL.absoluteString.UTF8String)) {
      owner->transferFailed(key->first, key->second, "Download did not return an archive response.", response ? response.statusCode : 0); return;
    }
    NSError *error = nil;
    [NSFileManager.defaultManager setAttributes:@{NSFileProtectionKey:NSFileProtectionCompleteUntilFirstUserAuthentication}
        ofItemAtPath:location.path error:&error];
    if (error) { failTransfer(*key, "Could not protect the downloaded file."); return; }
    if (owner->acceptFile(key->first, key->second, location.fileSystemRepresentation)) {
      std::error_code ignored; std::filesystem::remove(owner->jobPath(key->first) / "resume.data", ignored);
    }
  } catch (...) { /* A moved file is reconciled by the durable coordinator. */ }
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
  if (!error) return;
  try {
    const auto key = identity(task.taskDescription);
    if (!key) return;
    saveResume(*key, error.userInfo[NSURLSessionDownloadTaskResumeData]);
    failTransfer(*key, "Download interrupted. Retry to continue while the app is open.");
  } catch (...) {}
}
- (void)URLSessionDidFinishEventsForBackgroundURLSession:(NSURLSession *)session {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto completion = self.backgroundCompletion;
    self.backgroundCompletion = nil;
    if (completion) completion();
  });
}
@end

namespace downloads::platform {
std::filesystem::path privateRoot() {
  @autoreleasepool {
    NSURL *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *root = [support URLByAppendingPathComponent:@"Downloads" isDirectory:YES];
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:root withIntermediateDirectories:YES
        attributes:@{NSFileProtectionKey:NSFileProtectionCompleteUntilFirstUserAuthentication} error:&error] ||
        ![root setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&error])
      throw std::runtime_error("Private download storage is unavailable.");
    return root.fileSystemRepresentation;
  }
}
void initialize() { [host() reconcile]; }
std::string startTransfer(const Job &job, const std::filesystem::path &) {
  @autoreleasepool {
    const auto requestValue = downloads::transferRequest(job);
    NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:requestValue.url.c_str()]];
    if (!url) return "Invalid download URL.";
    auto owner = service();
    const auto resumePath = owner->jobPath(job.id) / "resume.data";
    NSData *resume = nil;
    std::error_code error;
    if (std::filesystem::is_regular_file(resumePath, error) && std::filesystem::file_size(resumePath, error) <= 4 * 1024 * 1024)
      resume = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:resumePath.c_str()]];
    // An invalid/expired token must not trap every retry in the same failure.
    if (resume) std::filesystem::remove(resumePath, error);
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    [request setValue:@"AsoBMaShow" forHTTPHeaderField:@"User-Agent"];
    for (const auto &[name, value] : requestValue.headers)
      [request setValue:[NSString stringWithUTF8String:value.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
    NSURLSession *session = persistentTransfer(requestValue) ? host().background : host().foreground;
    NSURLSessionDownloadTask *task = resume ? [session downloadTaskWithResumeData:resume] : [session downloadTaskWithRequest:request];
    task.taskDescription = descriptor(job);
    dispatchOwnedJob(dispatch_get_main_queue(), job, ^(const Job &snapshot) {
      [host() requestRuntime:snapshot];
    });
    [task resume];
    return {};
  }
}
void cancelTransfer(const Job &job) { [host() stop:descriptor(job)]; }
void beginImport(const Job &job) {
  dispatchOwnedJob(dispatch_get_main_queue(), job, ^(const Job &snapshot) {
    [host() requestRuntime:snapshot];
  });
}
void finishActivity(const Job &job, bool success) {
  NSString *description = descriptor(job);
  const auto revision = job.revision;
  dispatch_async(dispatch_get_main_queue(), ^{
    NSNumber *activeRevision = host().runtimeRevisions[description];
    if (activeRevision && activeRevision.unsignedLongLongValue > revision) return;
    [host() finishRuntime:description success:success];
  });
}
void reportProgress(const Job &job) {
  static std::mutex mutex; static std::chrono::steady_clock::time_point last;
  {
    std::lock_guard lock(mutex); const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(250)) return;
    last = now;
  }
  dispatchOwnedJob(dispatch_get_main_queue(), job, ^(const Job &snapshot) {
    if (@available(iOS 26.0, *)) {
      BGTask *task = host().continued[descriptor(snapshot)];
      if (![task isKindOfClass:BGContinuedProcessingTask.class]) return;
      auto continued = (BGContinuedProcessingTask *)task;
      const double fraction = snapshot.totalBytes ? std::min(1.0, static_cast<double>(snapshot.downloadedBytes) / snapshot.totalBytes) : 0;
      continued.progress.completedUnitCount = snapshot.state == State::Importing ? 700 + static_cast<int>(300 * fraction) : static_cast<int>(700 * fraction);
      [continued updateTitle:@"BMS download" subtitle:[NSString stringWithUTF8String:(snapshot.message.empty() ? stateName(snapshot.state) : snapshot.message.c_str())]];
    }
  });
}

void prepareApplicationLaunch() {
  // SDL owns UIApplicationDelegate. Extend its runtime class rather than link
  // private SDL framework symbols or depend on a renderer being created.
  SEL selector = @selector(application:handleEventsForBackgroundURLSession:completionHandler:);
  for (NSString *name in @[@"SDLUIKitDelegate", @"SDLUIKitSceneDelegate"]) {
    Class cls = NSClassFromString(name); if (!cls) continue;
    Method existing = class_getInstanceMethod(cls, selector);
    IMP previous = existing ? method_getImplementation(existing) : nullptr;
    IMP implementation = imp_implementationWithBlock(^(id receiver, UIApplication *application, NSString *identifier, void (^completion)(void)) {
      if ([identifier isEqualToString:sessionIdentifier]) {
        host().backgroundCompletion = completion;
        (void)host().background;
      } else if (previous) {
        ((void (*)(id, SEL, UIApplication *, NSString *, void (^)(void)))previous)(receiver, selector, application, identifier, completion);
      } else completion();
    });
    if (!class_addMethod(cls, selector, implementation, "v@:@@@?")) class_replaceMethod(cls, selector, implementation, "v@:@@@?");
  }
  [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:NSOperationQueue.mainQueue
      usingBlock:^(NSNotification *) {
    try { setGameForeground(false); } catch (...) {}
    [host().foreground getAllTasksWithCompletionHandler:^(NSArray<__kindof NSURLSessionTask *> *tasks) {
      dispatch_async(dispatch_get_main_queue(), ^{
        for (NSURLSessionTask *task in tasks) {
          if (host().continued[task.taskDescription]) continue;
          const auto key = identity(task.taskDescription); if (!key) continue;
          try { service()->interrupt(key->first, "Paused in background. Retry when the app is open."); } catch (...) {}
        }
      });
    }];
  }];
  [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
      usingBlock:^(NSNotification *) { try { setGameForeground(true); } catch (...) {} }];
}
} // namespace downloads::platform
#endif
