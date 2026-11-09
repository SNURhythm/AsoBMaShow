#include "../targets.h"
#if TARGET_OS_IPHONE
#include "RetiredBmsDownloadsIOS.h"
#import <UIKit/UIKit.h>
#import <BackgroundTasks/BackgroundTasks.h>
#import <objc/runtime.h>

namespace {
NSString *const retiredSessionIdentifier = @"com.snurhythm.AsoBMaShow.downloads.v1";
}

// Reattach only to invalidate the old OS-owned session, never to resume a job.
@interface RetiredBmsDownloadSession : NSObject <NSURLSessionDelegate>
@property(nonatomic, strong) NSURLSession *session;
@property(nonatomic, strong) NSMutableArray *completions;
@property(nonatomic) BOOL invalidated;
@end
@implementation RetiredBmsDownloadSession
- (instancetype)init {
  if ((self = [super init])) {
    self.completions = [NSMutableArray array];
    NSURLSessionConfiguration *configuration =
        [NSURLSessionConfiguration backgroundSessionConfigurationWithIdentifier:retiredSessionIdentifier];
    self.session = [NSURLSession sessionWithConfiguration:configuration delegate:self
                                            delegateQueue:NSOperationQueue.mainQueue];
    [self.session invalidateAndCancel];
  }
  return self;
}
- (void)URLSession:(NSURLSession *)session didBecomeInvalidWithError:(NSError *)error {
  self.invalidated = YES;
  self.session = nil;
  for (void (^completion)(void) in self.completions) completion();
  [self.completions removeAllObjects];
}
@end

namespace retired_bms_downloads {
void prepareApplicationLaunch() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    static RetiredBmsDownloadSession *retired = [RetiredBmsDownloadSession new];
    SEL selector = @selector(application:handleEventsForBackgroundURLSession:completionHandler:);
    for (NSString *name in @[@"SDLUIKitDelegate", @"SDLUIKitSceneDelegate"]) {
      Class cls = NSClassFromString(name);
      if (!cls) continue;
      Method existing = class_getInstanceMethod(cls, selector);
      IMP previous = existing ? method_getImplementation(existing) : nullptr;
      IMP implementation = imp_implementationWithBlock(^(id receiver, UIApplication *application,
          NSString *identifier, void (^completion)(void)) {
        if ([identifier isEqualToString:retiredSessionIdentifier]) {
          if (retired.invalidated) completion();
          else [retired.completions addObject:[completion copy]];
        } else if (previous) {
          ((void (*)(id, SEL, UIApplication *, NSString *, void (^)(void)))previous)(
              receiver, selector, application, identifier, completion);
        } else completion();
      });
      if (!class_addMethod(cls, selector, implementation, "v@:@@@?"))
        class_replaceMethod(cls, selector, implementation, "v@:@@@?");
    }
    NSString *prefix = [NSBundle.mainBundle.bundleIdentifier stringByAppendingString:@".downloads."];
    [BGTaskScheduler.sharedScheduler getPendingTaskRequestsWithCompletionHandler:^(NSArray<BGTaskRequest *> *requests) {
      for (BGTaskRequest *request in requests) {
        if ([request.identifier hasPrefix:prefix])
          [BGTaskScheduler.sharedScheduler cancelTaskRequestWithIdentifier:request.identifier];
      }
    }];
  });
}
}
#endif
