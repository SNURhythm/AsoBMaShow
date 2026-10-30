#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <cstdio>
#include <atomic>
#include <thread>
#include "platform/IOSSDLRefreshClock.h"

bool IsIOSMainThread() { return NSThread.isMainThread; }
void RunIOSMainThread(std::function<void()> operation) {
  if (NSThread.isMainThread) operation();
  else dispatch_sync(dispatch_get_main_queue(), ^{ operation(); });
}

static CADisplayLink *sdlLink;
static unsigned sdlTicks = 0;
static NSInteger advertisedRefresh = 120;
static id (*createLink)(id, SEL, id, SEL);
static void (*sdlTick)(id, SEL, CADisplayLink *);
static id trackLink(id cls, SEL sel, id target, SEL action) {
  id link = createLink(cls, sel, target, action);
  if ([NSStringFromClass([target class]) isEqualToString:@"SDL_uikitviewcontroller"]) sdlLink = link;
  return link;
}
static void trackTick(id target, SEL sel, CADisplayLink *link) {
  ++sdlTicks;
  sdlTick(target, sel, link);
}
static NSInteger simulatedProMotion(id, SEL) { return advertisedRefresh; }
static void require(bool value, const char *message) {
  if (!value) { NSLog(@"FAIL %s SDL=%s", message, SDL_GetError()); abort(); }
}
@interface Renderer : NSObject <CAMetalDisplayLinkDelegate>
@property(strong) id<MTLCommandQueue> queue;
@property(nonatomic) unsigned frames;
@end
@implementation Renderer
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
  @autoreleasepool {
    id<MTLCommandBuffer> command = [self.queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = update.drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(.02,.04,.08,1);
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder endEncoding];
    [command presentDrawable:update.drawable];
    [command commit];
    ++self.frames;
  }
}
@end
static void pumpUntil(bool (^condition)(void)) {
  const double end = CACurrentMediaTime() + 5;
  while (!condition() && CACurrentMediaTime() < end) {
    @autoreleasepool { SDL_PumpEvents(); CFRunLoopRunInMode(kCFRunLoopDefaultMode,.004,true); }
  }
  require(condition(), "native callback wait timed out");
}
static void switchClockFromWorker(SDL_Window *window, bool enabled) {
  std::atomic<bool> complete{false};
  auto *finished = &complete;
  std::thread worker([&] { SetIOSSDLRefreshClockEnabled(window, enabled); complete = true; });
  pumpUntil(^bool { return finished->load(); });
  worker.join();
}
static int audit(int, char **) {
  @autoreleasepool {
    if ([NSProcessInfo.processInfo.arguments containsObject:@"--60"]) advertisedRefresh = 60;
    Method factory = class_getClassMethod(CADisplayLink.class, @selector(displayLinkWithTarget:selector:));
    createLink = (decltype(createLink))method_setImplementation(factory, (IMP)trackLink);
    Class controller = NSClassFromString(@"SDL_uikitviewcontroller");
    Method tick = class_getInstanceMethod(controller, sel_registerName("doLoop:"));
    require(tick != nullptr,"actual SDL controller loaded");
    sdlTick = (decltype(sdlTick))method_setImplementation(tick,(IMP)trackTick);
    Method maxFPS = class_getInstanceMethod(UIScreen.class,@selector(maximumFramesPerSecond));
    IMP realFPS = method_setImplementation(maxFPS,(IMP)simulatedProMotion);
    require(SDL_Init(SDL_INIT_VIDEO),"SDL initializes");
    SDL_Window *window = SDL_CreateWindow("SDL clock audit",320,640,
        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    require(window != nullptr,"SDL window created");
    require((sdlLink != nil) == (advertisedRefresh > 60),
            "SDL automatically creates a display link only above 60 Hz");
    const float originalMin = sdlLink.preferredFrameRateRange.minimum;
    const float originalMax = sdlLink.preferredFrameRateRange.maximum;
    const float originalPreferred = sdlLink.preferredFrameRateRange.preferred;
    method_setImplementation(maxFPS, realFPS);
    SDL_MetalView view = SDL_Metal_CreateView(window);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view);
    layer.device = MTLCreateSystemDefaultDevice();
    layer.opaque = YES;
    layer.maximumDrawableCount = 3;
    Renderer *renderer = [Renderer new];
    renderer.queue = [layer.device newCommandQueue];
    CAMetalDisplayLink *metal = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    metal.delegate = renderer;
    metal.preferredFrameLatency = 1;
    metal.preferredFrameRateRange = CAFrameRateRangeMake(60,60,60);
    [metal addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    pumpUntil(^bool { return renderer.frames >= 20 && (advertisedRefresh == 60 || sdlTicks >= 10); });
    switchClockFromWorker(window, false);
    const unsigned beforeDisable = sdlTicks;
    const unsigned metalBeforeDisable = renderer.frames;
    pumpUntil(^bool { return renderer.frames >= metalBeforeDisable + 20; });
    require(sdlTicks == beforeDisable,"SDL clock remains stopped while Metal callbacks continue");
    const unsigned metalAfterDisable = renderer.frames;
    [metal invalidate];
    metal.delegate = nil;
    // Force only advertised capability again to inspect the fallback request.
    method_setImplementation(maxFPS,(IMP)simulatedProMotion);
    switchClockFromWorker(window, true);
    const float fallbackMin = sdlLink.preferredFrameRateRange.minimum;
    const float fallbackMax = sdlLink.preferredFrameRateRange.maximum;
    const float fallbackPreferred = sdlLink.preferredFrameRateRange.preferred;
    method_setImplementation(maxFPS,realFPS);
    if (advertisedRefresh > 60) {
      pumpUntil(^bool { return sdlTicks >= beforeDisable + 10; });
      require(fallbackPreferred == 120 && fallbackMax == 120,"fallback preserves the 120 Hz request");
    } else {
      require(sdlLink == nil && sdlTicks == 0,"60 Hz fallback does not create an extra clock");
    }
    require(renderer.frames == metalAfterDisable,"Metal link stays invalidated during fallback");
    NSDictionary *result = @{@"advertisedRefreshIsSynthetic":@YES, @"advertisedRefresh":@(advertisedRefresh),
        @"originalRange":@[@(originalMin),@(originalMax),@(originalPreferred)],
        @"fallbackRange":@[@(fallbackMin),@(fallbackMax),@(fallbackPreferred)],
        @"sdlTicksBeforeDisable":@(beforeDisable),@"metalFramesBeforeDisable":@(metalBeforeDisable),
        @"metalFramesAfterDisable":@(metalAfterDisable),@"sdlTicksAfterRestore":@(sdlTicks)};
    NSData *json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
    NSString *file = [NSString stringWithFormat:@"Documents/result-%ld.json",(long)advertisedRefresh];
    [json writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:file] atomically:YES];
    NSLog(@"PASS SDL coexistence, disable, Metal continuation and fallback restore: %@",result);
    SDL_SetiOSAnimationCallback(window,1,nullptr,nullptr);
    SDL_Metal_DestroyView(view);
    SDL_DestroyWindow(window);
    SDL_Quit();
  }
  return 0;
}
int main(int argc, char **argv) { return SDL_RunApp(argc,argv,audit,nullptr); }
