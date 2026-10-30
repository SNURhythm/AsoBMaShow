#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstdio>
#include "video/MetalDisplayLink.mm"
#include "../cmake/BgfxMetalDrawableProvider.cpp"

// Research-only wrapper records the actual OS update delivered to the real
// production delegate. No seeded callback, replacement timer, or forced flush.
struct Row { double callback, deadline, target, request, scheduled, displayed; };
static thread_local Row lastUpdate{};
static void (*originalUpdate)(id, SEL, CAMetalDisplayLink *, CAMetalDisplayLinkUpdate *);
static void recordUpdate(id receiver, SEL selector, CAMetalDisplayLink *link,
                         CAMetalDisplayLinkUpdate *update) {
  lastUpdate = {CACurrentMediaTime(), update.targetTimestamp,
                update.targetPresentationTimestamp, 0, 0, 0};
  originalUpdate(receiver, selector, link, update);
}
static std::mutex rowsMutex;
static std::vector<Row> rows;
static std::atomic<unsigned> frames{0}, failures{0};
static std::atomic<bool> done{false};
static bool nested = true;

@interface AuditView : UIView
@end
@implementation AuditView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

@interface AuditScene : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow *window;
@property(strong, nonatomic) CAMetalLayer *metal;
@property(strong, nonatomic) NSTimer *monitor;
@property(nonatomic) double started, pendingSince, maxCommitDelay;
@property(nonatomic) unsigned writes, reconciled, presentationMissing, checked;
- (void)observe;
- (void)finish;
@end
@implementation AuditScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options {
  nested = ![NSProcessInfo.processInfo.arguments containsObject:@"--standard"];
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  UIViewController *controller = [UIViewController new];
  controller.view = [[AuditView alloc] initWithFrame:self.window.bounds];
  self.window.rootViewController = controller;
  [self.window makeKeyAndVisible];
  [self.window layoutIfNeeded];
  self.metal = (CAMetalLayer *)controller.view.layer;
  self.metal.device = MTLCreateSystemDefaultDevice();
  self.metal.opaque = YES;
  self.metal.framebufferOnly = YES;
  self.metal.presentsWithTransaction = NO;
  self.metal.maximumDrawableCount = 3;
  self.metal.contentsScale = self.window.screen.nativeScale;
  self.metal.drawableSize = CGSizeMake(controller.view.bounds.size.width * self.metal.contentsScale,
                                      controller.view.bounds.size.height * self.metal.contentsScale);
  Method callback = class_getInstanceMethod(AsoMetalDisplayLinkDelegate.class,
      @selector(metalDisplayLink:needsUpdate:));
  originalUpdate = (decltype(originalUpdate))method_setImplementation(callback, (IMP)recordUpdate);
  dispatch_async(dispatch_get_main_queue(), ^{
    self.started = CACurrentMediaTime();
    CAMetalLayer *layer = self.metal;
    std::thread worker([layer] {
      @autoreleasepool {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        video::MetalDisplayLink link;
        id<MTLCommandQueue> queue = [layer.device newCommandQueue];
        id<MTLCommandBuffer> lastCommand = nil;
        const double end = CACurrentMediaTime() + 10;
        while (frames < 120 && CACurrentMediaTime() < end) {
          @autoreleasepool {
            if (!link.beginFrame((__bridge void *)layer, 60, 0, 1, [] { return true; })) {
              ++failures;
              // The app does timer-paced fallback work during retry cooldown.
              // Bound this harness's retries too, instead of busy spinning.
              std::this_thread::sleep_for(std::chrono::milliseconds(16));
              continue;
            }
            void *pointer = nullptr;
            if (!asobmashowMetalAcquireDrawable((__bridge void *)layer, &pointer) || !pointer) {
              ++failures;
              link.endFrame();
              continue;
            }
            id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)pointer;
            id<MTLCommandBuffer> command = [queue commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(.03, .05, .09, 1);
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            [encoder endEncoding];
            const unsigned index = frames.load();
            {
              std::lock_guard lock(rowsMutex);
              rows.push_back(lastUpdate);
              rows.back().request = CACurrentMediaTime();
            }
            [command addScheduledHandler:^(id<MTLCommandBuffer>) {
              std::lock_guard lock(rowsMutex);
              rows[index].scheduled = CACurrentMediaTime();
            }];
#if !TARGET_OS_SIMULATOR
            [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
              std::lock_guard lock(rowsMutex);
              rows[index].displayed = shown.presentedTime;
            }];
#endif
            if (asobmashowMetalShouldPresent((__bridge void *)layer, pointer))
              [command presentDrawable:drawable];
            else ++failures;
            [command commit];
            lastCommand = command;
            link.endFrame();
            ++frames;
          }
        }
        link.pause();
        [lastCommand waitUntilCompleted];
        done = true;
      }
    });
    if (nested) {
      while (!done) {
        @autoreleasepool {
          const double stop = CACurrentMediaTime() + .001;
          do {
            double remaining = stop - CACurrentMediaTime();
            if (remaining > 0) CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::min(.001, remaining), true);
            remaining = stop - CACurrentMediaTime();
            if (remaining > 0) CFRunLoopRunInMode((__bridge CFStringRef)UITrackingRunLoopMode,
                                                std::min(.001, remaining), true);
          } while (CACurrentMediaTime() < stop);
          [self observe];
        }
      }
      worker.join();
      [self finish];
    } else {
      worker.detach();
      self.monitor = [NSTimer scheduledTimerWithTimeInterval:.001 repeats:YES block:^(NSTimer *timer) {
        [self observe];
        if (done) { [timer invalidate]; self.monitor = nil; [self finish]; }
      }];
    }
  });
}
- (void)observe {
  ++self.checked;
  CALayer *presented = self.metal.presentationLayer;
  if (!presented) ++self.presentationMissing;
  if (self.pendingSince && presented && CGPointEqualToPoint(presented.position, self.metal.position)) {
    self.maxCommitDelay = std::max(self.maxCommitDelay, CACurrentMediaTime() - self.pendingSince);
    self.pendingSince = 0;
    ++self.reconciled;
  }
  if (!self.pendingSince && self.writes < 4 && frames >= 20 * (self.writes + 1)) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    CGPoint position = self.metal.position;
    position.x += self.writes % 2 ? -.5 : .5;
    self.metal.position = position;
    [CATransaction commit];
    self.pendingSince = CACurrentMediaTime();
    ++self.writes;
  }
}
- (void)finish {
  NSMutableArray *timings = [NSMutableArray array];
  {
    std::lock_guard lock(rowsMutex);
    for (const auto &r : rows) [timings addObject:@[@(r.callback), @(r.deadline), @(r.target),
        @(r.request), @(r.scheduled), @(r.displayed)]];
  }
  NSDictionary *result = @{@"nested":@(nested), @"frames":@(frames.load()), @"failures":@(failures.load()),
      @"writes":@(self.writes), @"reconciled":@(self.reconciled), @"maxCommitDelayMs":@(self.maxCommitDelay*1000),
      @"presentationMissing":@(self.presentationMissing), @"observations":@(self.checked), @"rows":timings};
  NSData *json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
  NSString *name = nested ? @"Documents/nested.json" : @"Documents/standard.json";
  [json writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:name] atomically:YES];
  NSLog(@"AUDIT %@ frames=%u failures=%u commits=%u/%u maxCommitDelayMs=%.3f",
      nested ? @"nested" : @"standard", frames.load(), failures.load(), self.reconciled, self.writes,
      self.maxCommitDelay * 1000);
}
@end
@interface AuditApp : UIResponder <UIApplicationDelegate>
@end
@implementation AuditApp
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options { return YES; }
@end
int main(int argc, char **argv) {
  @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"AuditApp"); }
}
