#include "IOSNativeMetalProbe.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#include "IOSApplicationRuntime.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <UIKit/UIKit.h>
#include <cmath>
#include <algorithm>
#include <SDL3/SDL_uikit_rawtouch.h>

namespace {
NSString *const kProbeRequest = @"AsoBMaShowNativeMetalProbeOnce";
NSInteger sSDLProbeMode = 0; // Main-thread startup only.
}

// Named subclass makes the probe distinguishable in copied layer snapshots.
@interface AsoNativeProbeMetalLayer : CAMetalLayer
@end
@implementation AsoNativeProbeMetalLayer
@end

@interface AsoNativeProbeView : UIView
@end
@implementation AsoNativeProbeView
+ (Class)layerClass { return AsoNativeProbeMetalLayer.class; }
@end

@interface AsoNativeProbeController : UIViewController
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@property(nonatomic, strong) id<MTLRenderPipelineState> pipeline;
@property(nonatomic, strong) CADisplayLink *displayLink;
@property(nonatomic, strong) CAMetalDisplayLink *metalDisplayLink API_AVAILABLE(ios(17.0));
@property(nonatomic, strong) NSObject *metalDisplayLinkDelegate;
@property(nonatomic) BOOL useMetalDisplayLink;
@property(nonatomic, strong) UIView *externalSurface;
@property(nonatomic, strong) id<MTLCommandBuffer> lastCommand;
@property(nonatomic) BOOL finished;
@property(nonatomic) NSUInteger frameCount;
@property(nonatomic) CFTimeInterval lastTimestamp;
@property(nonatomic) CFTimeInterval lastInterval;
- (BOOL)prepare:(NSString **)failure;
- (UIView *)surface;
- (BOOL)start:(NSString **)failure;
- (BOOL)canDraw;
- (void)tick:(CADisplayLink *)link;
- (void)drawDrawable:(id<CAMetalDrawable>)drawable timestamp:(CFTimeInterval)timestamp;
- (void)finish;
@end

API_AVAILABLE(ios(17.0))
@interface AsoNativeProbeMetalDelegate : NSObject <CAMetalDisplayLinkDelegate>
@property(nonatomic, weak) AsoNativeProbeController *renderer;
@end
@implementation AsoNativeProbeMetalDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link
            needsUpdate:(CAMetalDisplayLinkUpdate *)update {
  AsoNativeProbeController *renderer = self.renderer;
  if (renderer.metalDisplayLink != link) return;
  // A CAMetalDisplayLink owns acquisition; never call nextDrawable here.
  [renderer drawDrawable:update.drawable timestamp:CACurrentMediaTime()];
}
@end

@implementation AsoNativeProbeController
- (UIView *)surface { return self.externalSurface ?: self.view; }
- (void)loadView {
  self.view = [[AsoNativeProbeView alloc] initWithFrame:CGRectZero];
  self.view.opaque = YES;
  UITapGestureRecognizer *exit = [[UITapGestureRecognizer alloc]
      initWithTarget:self action:@selector(finish)];
  exit.numberOfTapsRequired = 2;
  [self.view addGestureRecognizer:exit];
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)shouldAutorotate { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad
      ? UIInterfaceOrientationMaskAll : UIInterfaceOrientationMaskAllButUpsideDown;
}
- (BOOL)prepare:(NSString **)failure {
  self.device = MTLCreateSystemDefaultDevice();
  self.queue = [self.device newCommandQueue];
  if (self.queue == nil) {
    *failure = @"Metal device/command queue unavailable";
    return NO;
  }
  NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
      "vertex float4 probeVertex(uint id [[vertex_id]], constant float4 &params [[buffer(0)]]) {\n"
      "  float2 p[3] = {float2(-0.25,-0.25), float2(0.25,-0.25), float2(0,0.25)};\n"
      "  return float4((p[id] + float2(sin(params.x)*0.5,0)) * params.yz, 0, 1);\n}\n"
      "fragment float4 probeFragment() { return float4(0.2,0.9,0.6,1); }\n";
  NSError *error = nil;
  id<MTLLibrary> library = [self.device newLibraryWithSource:source options:nil error:&error];
  if (library == nil) {
    *failure = error.localizedDescription ?: @"Metal library unavailable";
    return NO;
  }
  MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
  descriptor.vertexFunction = [library newFunctionWithName:@"probeVertex"];
  descriptor.fragmentFunction = [library newFunctionWithName:@"probeFragment"];
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  self.pipeline = [self.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (self.pipeline == nil) {
    *failure = error.localizedDescription ?: @"Metal pipeline unavailable";
    return NO;
  }
  CAMetalLayer *layer = (CAMetalLayer *)self.surface.layer;
  layer.device = self.device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.opaque = YES;
  layer.framebufferOnly = YES;
  layer.presentsWithTransaction = NO;
  layer.maximumDrawableCount = 3;
  CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  layer.colorspace = colorSpace;
  CGColorSpaceRelease(colorSpace);
  return YES;
}
- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
  const CGFloat scale = self.view.window.screen.nativeScale;
  if (scale <= 0) return;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  layer.contentsScale = scale;
  layer.drawableSize = CGSizeMake(std::round(self.view.bounds.size.width * scale),
                                 std::round(self.view.bounds.size.height * scale));
  [CATransaction commit];
}
- (BOOL)start:(NSString **)failure {
  const float rate = self.surface.window.screen.maximumFramesPerSecond;
  if (self.useMetalDisplayLink) {
    if (@available(iOS 17.0, *)) {
      self.metalDisplayLink = [[CAMetalDisplayLink alloc]
          initWithMetalLayer:(CAMetalLayer *)self.surface.layer];
      if (self.metalDisplayLink == nil) {
        *failure = @"CAMetalDisplayLink unavailable";
        return NO;
      }
      AsoNativeProbeMetalDelegate *delegate = [AsoNativeProbeMetalDelegate new];
      delegate.renderer = self;
      self.metalDisplayLinkDelegate = delegate;
      self.metalDisplayLink.delegate = delegate;
      self.metalDisplayLink.preferredFrameLatency = 1.0F;
      self.metalDisplayLink.preferredFrameRateRange = CAFrameRateRangeMake(rate, rate, rate);
      [self.metalDisplayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
      self.metalDisplayLink.paused = NO;
      return YES;
    }
    *failure = @"CAMetalDisplayLink requires iOS 17 or later";
    return NO; // Do not silently substitute a different experiment.
  }
  self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
  self.displayLink.preferredFrameRateRange = CAFrameRateRangeMake(rate, rate, rate);
  [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
  return YES;
}
- (BOOL)canDraw {
  CAMetalLayer *layer = (CAMetalLayer *)self.surface.layer;
  return !self.finished &&
      UIApplication.sharedApplication.applicationState == UIApplicationStateActive &&
      self.surface.window.windowScene.activationState == UISceneActivationStateForegroundActive &&
      layer.drawableSize.width > 0 && layer.drawableSize.height > 0;
}
- (void)tick:(CADisplayLink *)link {
  @autoreleasepool {
    if (![self canDraw]) return;
    CAMetalLayer *layer = (CAMetalLayer *)self.surface.layer;
    [self drawDrawable:[layer nextDrawable] timestamp:link.timestamp];
  }
}
- (void)drawDrawable:(id<CAMetalDrawable>)drawable timestamp:(CFTimeInterval)timestamp {
  @autoreleasepool {
    if (drawable == nil || ![self canDraw]) return;
    id<MTLCommandBuffer> command = [self.queue commandBuffer];
    if (command == nil) return;
    command.label = @"Native Metal probe (no bgfx)";
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.02, 0.06, 0.16, 1);
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) return;
    const float width = drawable.texture.width;
    const float height = drawable.texture.height;
    const float shortSide = std::min(width, height);
    const float params[4] = {static_cast<float>(std::fmod(timestamp, 6.283185307)),
                             shortSide / width, shortSide / height, 0};
    [encoder setRenderPipelineState:self.pipeline];
    [encoder setVertexBytes:params length:sizeof(params) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command presentDrawable:drawable];
    [command commit];
    self.lastCommand = command;
    if (self.lastTimestamp != 0) self.lastInterval = timestamp - self.lastTimestamp;
    self.lastTimestamp = timestamp;
    ++self.frameCount;
  }
}
- (void)finish {
  // Snapshot before removing the native window. No UIKit overlay is added.
  if (CopyIOSMetalPresentationDiagnostics()) {
    UIPasteboard.generalPasteboard.string = [NSString stringWithFormat:
        @"Native Metal probe: %@; bgfx not initialized.\n"
         "scheduler=%@ drawableSource=%@ preferredFrameLatency=%@ callbackClock=%@\n"
         "submittedFrames=%lu lastCallbackIntervalMs=%.3f (not presentation latency)\n%@",
        self.externalSurface ? @"SDL-created surface" : @"native window (SDL not initialized)",
        self.useMetalDisplayLink ? @"CAMetalDisplayLink" : @"CADisplayLink",
        self.useMetalDisplayLink ? @"update.drawable" : @"nextDrawable",
        self.useMetalDisplayLink ? @"1" : @"n/a",
        self.useMetalDisplayLink ? @"CACurrentMediaTime" : @"CADisplayLink.timestamp",
        (unsigned long)self.frameCount, self.lastInterval * 1000,
        UIPasteboard.generalPasteboard.string];
  }
  [self.displayLink invalidate];
  self.displayLink = nil;
  if (@available(iOS 17.0, *)) {
    [self.metalDisplayLink invalidate];
    self.metalDisplayLink.delegate = nil;
    self.metalDisplayLink = nil;
  }
  self.metalDisplayLinkDelegate = nil;
  // Teardown only: the application may initialize bgfx on this same layer next.
  // Never wait for GPU completion in the measured per-frame path.
  [self.lastCommand waitUntilCompleted];
  self.lastCommand = nil;
  self.finished = YES;
}
@end

bool ArmIOSNativeMetalProbe(IOSNativeMetalProbeMode mode) {
  return platform::onMain([mode] {
    [NSUserDefaults.standardUserDefaults setInteger:static_cast<NSInteger>(mode) forKey:kProbeRequest];
    // Persist before the tester explicitly terminates the app for a cold start.
    return static_cast<bool>([NSUserDefaults.standardUserDefaults synchronize]);
  });
}

void RunIOSNativeMetalProbeIfRequested() {
  NSCAssert(NSThread.isMainThread, @"Native probe must run before application startup on main");
  @autoreleasepool {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    const NSInteger mode = [defaults integerForKey:kProbeRequest];
    if (mode == 0) return;
    [defaults removeObjectForKey:kProbeRequest];
    [defaults synchronize]; // Consume first: force-quit/crash cannot trap the next launch.
    if (mode == static_cast<NSInteger>(IOSNativeMetalProbeMode::SDLSurface) ||
        mode == static_cast<NSInteger>(IOSNativeMetalProbeMode::SDLMetalDisplayLink)) {
      sSDLProbeMode = mode;
      return; // Run only after normal startup creates its SDL Metal view.
    }
    UIWindowScene *scene = nil;
    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes) {
      if ([candidate isKindOfClass:UIWindowScene.class] &&
          candidate.activationState != UISceneActivationStateBackground &&
          candidate.activationState != UISceneActivationStateUnattached) {
        scene = (UIWindowScene *)candidate;
        break;
      }
    }
    NSString *failure = nil;
    AsoNativeProbeController *controller = [AsoNativeProbeController new];
    controller.useMetalDisplayLink =
        mode == static_cast<NSInteger>(IOSNativeMetalProbeMode::NativeMetalDisplayLink);
    if (scene == nil || ![controller prepare:&failure]) {
      UIPasteboard.generalPasteboard.string = [@"Native Metal probe could not start: "
          stringByAppendingString:failure ?: @"no connected foreground window scene"];
      return;
    }
    // No SDL window exists yet. Keep launch/other app windows out of the test.
    NSArray<UIWindow *> *previousWindows = [scene.windows copy];
    for (UIWindow *window in previousWindows) window.hidden = YES;
    UIWindow *probeWindow = [[UIWindow alloc] initWithWindowScene:scene];
    probeWindow.rootViewController = controller;
    [probeWindow makeKeyAndVisible];
    [probeWindow layoutIfNeeded];
    if (![controller start:&failure]) {
      UIPasteboard.generalPasteboard.string = failure;
      probeWindow.hidden = YES;
      probeWindow.rootViewController = nil;
      return;
    }
    // UIKit still handles rotation, activity and gestures. There is no SDL
    // event pump, app/render worker, audio engine, or bgfx renderer in this loop.
    while (!controller.finished) {
      @autoreleasepool {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
      }
    }
    probeWindow.hidden = YES;
    probeWindow.rootViewController = nil;
    // Do not resurrect a launch window that the bootstrap retired while the
    // run loop was serviced. Normal startup creates its own SDL window next.
  }
}

bool RunIOSSDLMetalProbeIfRequested(void *metalLayer) {
  NSCAssert(NSThread.isMainThread, @"SDL surface probe must run before bgfx on main");
  if (sSDLProbeMode == 0) return false;
  const NSInteger mode = sSDLProbeMode;
  sSDLProbeMode = 0;
  @autoreleasepool {
    CAMetalLayer *layer = (__bridge CAMetalLayer *)metalLayer;
    UIView *surface = [layer.delegate isKindOfClass:UIView.class] ? (UIView *)layer.delegate : nil;
    if (![layer isKindOfClass:CAMetalLayer.class] || surface.window == nil) {
      UIPasteboard.generalPasteboard.string = @"SDL Metal probe could not find its attached surface.";
      return false;
    }
    AsoNativeProbeController *renderer = [AsoNativeProbeController new];
    // Do not replace the SDL view, root controller, window, or its layout code.
    renderer.externalSurface = surface;
    renderer.useMetalDisplayLink =
        mode == static_cast<NSInteger>(IOSNativeMetalProbeMode::SDLMetalDisplayLink);
    NSString *failure = nil;
    if (![renderer prepare:&failure]) {
      UIPasteboard.generalPasteboard.string = [@"SDL Metal probe could not start: "
          stringByAppendingString:failure ?: @"unknown Metal setup failure"];
      return false;
    }
    UITapGestureRecognizer *exit = [[UITapGestureRecognizer alloc]
        initWithTarget:renderer action:@selector(finish)];
    exit.numberOfTapsRequired = 2;
    [surface addGestureRecognizer:exit];
    if (![renderer start:&failure]) {
      [surface removeGestureRecognizer:exit];
      UIPasteboard.generalPasteboard.string = [@"SDL Metal probe could not start: "
          stringByAppendingString:failure];
      return false;
    }
    while (!renderer.finished) {
      @autoreleasepool {
        SDL_PumpEvents();
        // Preserve lifecycle, quit, device and file-open events for startup.
        SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_WHEEL);
        SDL_FlushEvents(SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_CANCELED);
        SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
      }
    }
    [surface removeGestureRecognizer:exit];
    // Discard probe touches before the normal application input owner starts.
    SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_WHEEL);
    SDL_FlushEvents(SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_CANCELED);
    SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP);
    IOSRawTouchEvent touches[64];
    while (IOSPopRawTouchEvents(touches, 64) != 0) {}
    return true;
  }
}
#endif
