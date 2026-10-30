#include "IOSNativeMetalProbe.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#include "IOSApplicationRuntime.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#include <cmath>

namespace {
NSString *const kProbeRequest = @"AsoBMaShowNativeMetalProbeOnce";
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
@property(nonatomic) BOOL finished;
@property(nonatomic) NSUInteger frameCount;
@property(nonatomic) CFTimeInterval lastTimestamp;
@property(nonatomic) CFTimeInterval lastInterval;
- (BOOL)prepare:(NSString **)failure;
- (void)start;
- (void)tick:(CADisplayLink *)link;
- (void)finish;
@end

@implementation AsoNativeProbeController
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
      "vertex float4 probeVertex(uint id [[vertex_id]], constant float &phase [[buffer(0)]]) {\n"
      "  float2 p[3] = {float2(-0.25,-0.25), float2(0.25,-0.25), float2(0,0.25)};\n"
      "  return float4(p[id] + float2(sin(phase)*0.5,0), 0, 1);\n}\n"
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
  CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
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
- (void)start {
  self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
  const float rate = self.view.window.screen.maximumFramesPerSecond;
  self.displayLink.preferredFrameRateRange = CAFrameRateRangeMake(rate, rate, rate);
  [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
- (void)tick:(CADisplayLink *)link {
  @autoreleasepool {
    if (self.finished || UIApplication.sharedApplication.applicationState != UIApplicationStateActive ||
        self.view.window.windowScene.activationState != UISceneActivationStateForegroundActive) return;
    CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
    if (layer.drawableSize.width <= 0 || layer.drawableSize.height <= 0) return;
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable == nil) return;
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
    float phase = static_cast<float>(std::fmod(link.timestamp, 6.283185307));
    [encoder setRenderPipelineState:self.pipeline];
    [encoder setVertexBytes:&phase length:sizeof(phase) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command presentDrawable:drawable];
    [command commit];
    if (self.lastTimestamp != 0) self.lastInterval = link.timestamp - self.lastTimestamp;
    self.lastTimestamp = link.timestamp;
    ++self.frameCount;
  }
}
- (void)finish {
  // Snapshot before removing the native window. No UIKit overlay is added.
  if (CopyIOSMetalPresentationDiagnostics()) {
    UIPasteboard.generalPasteboard.string = [NSString stringWithFormat:
        @"Native Metal cold-launch probe; SDL window/bgfx not initialized.\n"
         "submittedFrames=%lu lastCallbackIntervalMs=%.3f (not presentation latency)\n%@",
        (unsigned long)self.frameCount, self.lastInterval * 1000,
        UIPasteboard.generalPasteboard.string];
  }
  [self.displayLink invalidate];
  self.displayLink = nil;
  self.finished = YES;
}
@end

bool ArmIOSNativeMetalProbe() {
  return platform::onMain([] {
    [NSUserDefaults.standardUserDefaults setBool:YES forKey:kProbeRequest];
    // Persist before the tester explicitly terminates the app for a cold start.
    return static_cast<bool>([NSUserDefaults.standardUserDefaults synchronize]);
  });
}

void RunIOSNativeMetalProbeIfRequested() {
  NSCAssert(NSThread.isMainThread, @"Native probe must run before application startup on main");
  @autoreleasepool {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if (![defaults boolForKey:kProbeRequest]) return;
    [defaults removeObjectForKey:kProbeRequest];
    [defaults synchronize]; // Consume first: force-quit/crash cannot trap the next launch.
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
    [controller start];
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
#endif
