// Research-only: real Simulator callbacks through the production link/provider.
// See README.md. Not application code or a physical scanout test.
#import <UIKit/UIKit.h>
#import <IOSurface/IOSurfaceRef.h>
#include <atomic>
#include <thread>
#include <map>
#include "video/MetalDisplayLink.mm"
#include "../cmake/BgfxMetalDrawableProvider.cpp"

static std::atomic<bool> done{false};
static NSMutableArray *rows;
static unsigned beginRetries = 0, rejectedDrawables = 0;

@interface AuditView : UIView
@end
@implementation AuditView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

@interface AuditScene : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow *window;
@property(strong, nonatomic) NSTimer *monitor;
@end
@implementation AuditScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options {
  rows = [NSMutableArray array];
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
  dispatch_async(dispatch_get_main_queue(), ^{
    std::thread([layer] {
      @autoreleasepool {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        video::MetalDisplayLink link;
        id<MTLCommandQueue> queue = [layer.device newCommandQueue];
        id<MTLCommandBuffer> lastCommand = nil;
        std::map<IOSurfaceID, IOSurfaceRef> surfaces;
        unsigned frames = 0;
        const double end = CACurrentMediaTime() + 10;
        while (frames < 160 && CACurrentMediaTime() < end) {
          @autoreleasepool {
            if (!link.beginFrame((__bridge void *)layer, 60, 0, 1, [] { return true; })) {
              ++beginRetries;
              // This includes cooldown polls; it is not a missed-frame count.
              std::this_thread::sleep_for(std::chrono::milliseconds(16));
              continue;
            }
            void *pointer = nullptr;
            if (!asobmashowMetalAcquireDrawable((__bridge void *)layer, &pointer) || !pointer) {
              ++rejectedDrawables;
              link.endFrame();
              continue;
            }
            id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)pointer;
            IOSurfaceRef surface = drawable.texture.iosurface;
            const IOSurfaceID surfaceId = surface ? IOSurfaceGetID(surface) : 0;
            // Keep only IOSurfaces alive, not textures/drawables. Otherwise a
            // freed surface ID may be recycled and falsely imply pool reuse.
            if (surface && !surfaces.count(surfaceId))
              surfaces.emplace(surfaceId, (IOSurfaceRef)CFRetain(surface));
            [rows addObject:@{@"frame":@(frames), @"surface":@(surfaceId),
                @"width":@(drawable.texture.width), @"height":@(drawable.texture.height),
                @"serial":@(video::current->creationSerial)}];
            id<MTLCommandBuffer> command = [queue commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(.03, .05, .09, 1);
            [[command renderCommandEncoderWithDescriptor:pass] endEncoding];
            if (asobmashowMetalShouldPresent((__bridge void *)layer, pointer))
              [command presentDrawable:drawable];
            else ++rejectedDrawables;
            [command commit];
            lastCommand = command;
            link.endFrame();
            ++frames;
            if (frames == 40) video::MetalDisplayLink::requestRecreationForDiagnostics();
            if (frames == 80 || frames == 120) {
              const bool swapped = frames == 80;
              dispatch_sync(dispatch_get_main_queue(), ^{
                layer.drawableSize = swapped ? CGSizeMake(32, 48) : CGSizeMake(48, 32);
              });
            }
          }
        }
        link.pause();
        [lastCommand waitUntilCompleted];
        for (const auto &[id, surface] : surfaces) CFRelease(surface);
        done = true;
      }
    }).detach();
    self.monitor = [NSTimer scheduledTimerWithTimeInterval:.01 repeats:YES block:^(NSTimer *timer) {
      if (!done) return;
      [timer invalidate];
      self.monitor = nil;
      NSDictionary *result = @{@"beginRetries":@(beginRetries),
          @"rejectedDrawables":@(rejectedDrawables), @"rows":rows};
      NSData *json = [NSJSONSerialization dataWithJSONObject:result
          options:NSJSONWritingPrettyPrinted error:nil];
      [json writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/pool.json"]
          atomically:YES];
      NSLog(@"POOL AUDIT frames=%lu beginRetries=%u rejectedDrawables=%u",
          (unsigned long)rows.count, beginRetries, rejectedDrawables);
    }];
  });
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
