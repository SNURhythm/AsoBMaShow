#import <UIKit/UIKit.h>
#include <atomic>
#include <thread>
#include "video/MetalDisplayLink.mm"
#include "platform/IOSApplicationLifecycle.h"
#include "../cmake/BgfxMetalDrawableProvider.cpp"

static platform::IOSApplicationLifecycle lifecycle;
static std::atomic<bool> sceneActive{false};
static void observe(Uint32 type) {
  SDL_Event event{};
  event.type = type;
  lifecycle.observe(event, CACurrentMediaTime());
}
static void saveReport(const char *name, unsigned frames, unsigned retries) {
  NSMutableArray *sessions = [NSMutableArray array];
  for (const auto &capture : video::current->timing) {
    unsigned requests = 0, scheduled = 0, displayed = 0;
    const auto samples = capture.samples();
    for (const auto &sample : samples) {
      requests += sample->request.load() > 0;
      scheduled += sample->scheduledObserved.load() > 0;
      displayed += sample->presented.load() > 0;
    }
    [sessions addObject:@{@"serial":@(capture.serial), @"generation":@(capture.generation),
        @"samples":@(samples.size()), @"requests":@(requests), @"scheduled":@(scheduled),
        @"displayed":@(displayed), @"attached":@(capture.attachedAt), @"detached":@(capture.detachedAt)}];
  }
  NSMutableArray *events = [NSMutableArray array];
  for (const auto &event : lifecycle.diagnostics())
    [events addObject:@{@"type":@(event.type), @"at":@(event.observedAt),
        @"generation":@(event.presentation.generation), @"active":@(event.presentation.active)}];
  NSDictionary *result = @{@"framesInPhase":@(frames), @"retryPolls":@(retries),
      @"sessions":sessions, @"lifecycle":events,
      @"report":@(video::MetalDisplayLink::diagnosticsForCurrentThread().c_str()),
      @"limitation":@"Simulator cannot supply presentedTime or physical Direct/Composited evidence"};
  NSData *json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
  [json writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:
      [NSString stringWithFormat:@"Documents/%s.json", name]] atomically:YES];
  NSLog(@"RESUME AUDIT %s frames=%u sessions=%lu", name, frames, (unsigned long)sessions.count);
}

@interface AuditView : UIView
@end
@implementation AuditView
+ (Class)layerClass { return CAMetalLayer.class; }
@end
@interface AuditScene : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow *window;
@end
@implementation AuditScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options {
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  UIViewController *controller = [UIViewController new];
  controller.view = [[AuditView alloc] initWithFrame:self.window.bounds];
  self.window.rootViewController = controller;
  [self.window makeKeyAndVisible];
  [self.window layoutIfNeeded];
  CAMetalLayer *layer = (CAMetalLayer *)controller.view.layer;
  layer.device = MTLCreateSystemDefaultDevice();
  layer.opaque = YES;
  layer.framebufferOnly = YES;
  layer.presentsWithTransaction = NO;
  layer.maximumDrawableCount = 3;
  layer.drawableSize = CGSizeMake(48, 32);
  std::thread([layer] {
    @autoreleasepool {
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
      video::MetalDisplayLink link;
      id<MTLCommandQueue> queue = [layer.device newCommandQueue];
      unsigned frames = 0, retries = 0;
      std::uint64_t generation = 0;
      const double limit = CACurrentMediaTime() + 90;
      while (CACurrentMediaTime() < limit) {
        @autoreleasepool {
          const auto state = lifecycle.presentation();
          if (!sceneActive || !state.active) {
            link.pause();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
          }
          if (generation != state.generation) { generation = state.generation; frames = 0; }
          if (!link.beginFrame((__bridge void *)layer, 60, 0, generation, [generation] {
            const auto now = lifecycle.presentation();
            return sceneActive && now.active && now.generation == generation;
          })) {
            ++retries;
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
          }
          void *pointer = nullptr;
          if (asobmashowMetalAcquireDrawable((__bridge void *)layer, &pointer) && pointer) {
            id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)pointer;
            id<MTLCommandBuffer> command = [queue commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(.03, .05, .09, 1);
            [[command renderCommandEncoderWithDescriptor:pass] endEncoding];
            if (asobmashowMetalShouldPresent((__bridge void *)layer, pointer)) {
              asobmashowMetalWillPresent((__bridge void *)layer, pointer, (__bridge void *)command);
              [command presentDrawable:drawable];
            }
            [command commit];
            ++frames;
          }
          link.endFrame();
          if (frames == 160) {
            if (generation == 0) saveReport("initial", frames, retries);
            else {
              link.pause();
              saveReport("resumed", frames, retries);
              break;
            }
          }
        }
      }
    }
  }).detach();
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
  observe(SDL_EVENT_DID_ENTER_FOREGROUND);
  sceneActive = true;
}
- (void)sceneWillResignActive:(UIScene *)scene {
  sceneActive = false;
  observe(SDL_EVENT_WILL_ENTER_BACKGROUND);
}
- (void)sceneDidEnterBackground:(UIScene *)scene { observe(SDL_EVENT_DID_ENTER_BACKGROUND); }
- (void)sceneWillEnterForeground:(UIScene *)scene { observe(SDL_EVENT_WILL_ENTER_FOREGROUND); }
@end
@interface AuditApp : UIResponder <UIApplicationDelegate>
@end
@implementation AuditApp
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options { return YES; }
@end
int main(int argc, char **argv) {
  @autoreleasepool { return UIApplicationMain(argc, argv, nil, @"AuditApp"); }
}
