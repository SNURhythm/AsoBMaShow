#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include "video/MetalDrawableProvider.h"
#include "../src/video/MetalDisplayLink.mm"
#include <bgfx/bgfx.h>
#include <cstdio>
#include <cstdlib>

@interface AsoDrawableProbeLayer : CAMetalLayer
@property(nonatomic) NSUInteger acquisitions;
@end
@implementation AsoDrawableProbeLayer
- (id<CAMetalDrawable>)nextDrawable {
  ++_acquisitions;
  return [super nextDrawable];
}
@end

namespace {
void require(bool value, const char *message) {
  if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
void *supplied = nullptr;
unsigned providerCalls = 0, presentationCalls = 0;
bool allowPresentation = true;
bool acquire(void *, void **drawable) {
  ++providerCalls;
  *drawable = supplied;
  return true;
}
bool present(void *, void *drawable) {
  require(drawable == supplied, "bgfx presents the provider's drawable");
  ++presentationCalls;
  return allowPresentation;
}
const video::MetalDrawableProvider provider{acquire, present};
}

int main() {
  @autoreleasepool {
    require(!video::MetalDisplayLink::requestRecreationForDiagnostics(),
            "diagnostic recreation cannot run without an owner on this thread");
    auto *layer = [AsoDrawableProbeLayer new];
    bgfx::renderFrame();
    bgfx::Init init;
    init.type = bgfx::RendererType::Metal;
    init.fallback = false;
    init.platformData.nwh = (__bridge void *)layer;
    init.resolution.reset = BGFX_RESET_FLIP_AFTER_RENDER;
    init.resolution.width = 16;
    init.resolution.height = 16;
    require(bgfx::init(init), "native Metal initializes");
    bgfx::setViewRect(0, 0, 0, 16, 16);
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x123456ff);
    asobmashowMetalSetDrawableProvider(&provider);
    bgfx::touch(0);
    bgfx::frame();
    require(providerCalls == 1 && layer.acquisitions == 0,
            "handled nil drawable must not fall back to nextDrawable");
    require(presentationCalls == 0, "missing drawable must not be presented");

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    require(drawable != nil, "native layer supplies a drawable for handoff");
    supplied = (__bridge void *)drawable;
    const auto acquired = layer.acquisitions;
    allowPresentation = false;
    bgfx::touch(0);
    bgfx::frame();
    require(providerCalls == 2 && presentationCalls == 1 &&
                layer.acquisitions == acquired,
            "borrowed drawable is checked at presentation without reacquisition");
    allowPresentation = true;
    bgfx::touch(0);
    bgfx::frame();
    require(providerCalls == 3 && presentationCalls == 2 && layer.acquisitions == acquired,
            "valid borrowed provider drawable uses ordinary command-buffer presentation");
    supplied = nullptr;
    drawable = nil;

    asobmashowMetalSetDrawableProvider(nullptr);
    bgfx::touch(0);
    bgfx::frame();
    require(layer.acquisitions == acquired + 1,
            "removing provider restores native drawable acquisition");
    if (@available(macOS 14.0, iOS 17.0, *)) {
      video::MetalDisplayLink link;
      // White-box native fixture: use a real attached CAMetalDisplayLink and
      // real drawable, but seed the lease deterministically. No claim of native
      // callback delivery: a headless layer need not receive display updates.
      id<CAMetalDrawable> leased = [layer nextDrawable];
      require(leased != nil, "native drawable available before attaching link");
      auto &state = *video::current;
      state.layer = layer;
      state.link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
      require(state.link != nil, "native link attaches for deterministic ownership checks");
      ((CAMetalDisplayLink *)state.link).paused = YES;
      require(video::MetalDisplayLink::requestRecreationForDiagnostics(),
              "diagnostic recreation accepts an attached owner-thread link");
      id originalLink = state.link;
      require(video::MetalDisplayLink::requestRecreationForDiagnostics() &&
                  state.manualRequests == 1 && state.link == originalLink,
              "repeated requests coalesce without interrupting the current link");
      require(video::MetalDisplayLink::diagnosticsForCurrentThread().find(
                  "manualRequests=1 manualRecreations=0 recreationPending=1") != std::string::npos,
              "snapshot distinguishes queued recreation from completed recreation");
      const auto attachedAcquisitions = layer.acquisitions;
      const auto render = [] { bgfx::touch(0); bgfx::frame(); };
      // A link attached between iterations still owns acquisition.
      link.endFrame();
      render();
      require(layer.acquisitions == attachedAcquisitions && !link.didSchedulePresent(),
              "attached idle lease must return handled nil without nextDrawable");
      bool active = true;
      state.generation = 7;
      state.isCurrent = [&] { return active; };
      state.frame.begin(7);
      require(state.frame.receive(10.0, 10.008), "synthetic fresh callback seeds lease");
      state.drawable = leased;
      render();
      require(link.didSchedulePresent() && layer.acquisitions == attachedAcquisitions,
              "accepted lease presents normally without reacquiring");
      render();
      require(layer.acquisitions == attachedAcquisitions,
              "consumed lease remains managed nil on repeated acquisition");
      link.endFrame();
      render();
      require(layer.acquisitions == attachedAcquisitions && !link.didSchedulePresent(),
              "endFrame must preserve attached layer ownership");
      require(state.link == originalLink && state.recreationRequested,
              "queued recreation does not detach a current or retiring frame");
      const auto sizeBefore = layer.drawableSize;
      const auto countBefore = layer.maximumDrawableCount;
      NSString *filterBefore = layer.magnificationFilter;
      auto *owner = &state;
      // Supply one deterministic wake on the real owner run loop. Headless
      // layers need not receive real CAMetalDisplayLink callbacks.
      CFRunLoopTimerRef wake = CFRunLoopTimerCreateWithHandler(kCFAllocatorDefault,
          CFAbsoluteTimeGetCurrent(), 0, 0, 0, ^(CFRunLoopTimerRef) {
        const double now = CACurrentMediaTime();
        if (owner->frame.receive(now, now + 1.0)) owner->drawable = leased;
      });
      CFRunLoopAddTimer(CFRunLoopGetCurrent(), wake, kCFRunLoopDefaultMode);
      const bool recreated = link.beginFrame((__bridge void *)layer, 120, 0, 7,
                                             [&] { return active; });
      CFRunLoopTimerInvalidate(wake);
      CFRelease(wake);
      require(recreated && state.link != originalLink && state.link != nil &&
                  state.creationSerial == 1 && state.manualRecreations == 1 &&
                  !state.recreationRequested,
              "next frame replaces the link exactly once before obtaining its lease");
      require(layer.acquisitions == attachedAcquisitions &&
                  CGSizeEqualToSize(layer.drawableSize, sizeBefore) &&
                  layer.maximumDrawableCount == countBefore &&
                  [layer.magnificationFilter isEqualToString:filterBefore] &&
                  ((CAMetalDisplayLink *)state.link).preferredFrameLatency == 1.0F &&
                  ((CAMetalDisplayLink *)state.link).preferredFrameRateRange.preferred == 120,
              "recreation preserves drawable ownership, filter, geometry and pacing requests");
      require(video::MetalDisplayLink::diagnosticsForCurrentThread().find(
                  "manualRequests=1 manualRecreations=1 recreationPending=0") != std::string::npos,
              "snapshot confirms completed replacement");
      link.endFrame(); // Do not present the fixture's already-presented drawable again.
      state.generation = 8;
      state.isCurrent = [&] { return active; };
      state.frame.begin(8);
      state.frame.receive(11.0, 11.008);
      state.drawable = leased;
      active = false;
      render();
      require(layer.acquisitions == attachedAcquisitions && !link.didSchedulePresent(),
              "lifecycle rejection must not fall through to nextDrawable");
      require(video::MetalDisplayLink::requestRecreationForDiagnostics(),
              "another request can be queued before lifecycle cancellation");
      link.pause();
      require(!state.recreationRequested && state.manualRecreations == 1 &&
                  !video::MetalDisplayLink::requestRecreationForDiagnostics(),
              "lifecycle pause cancels pending recreation without reporting completion");
      render();
      require(layer.acquisitions == attachedAcquisitions + 1,
              "pause detaches and restores ordinary acquisition");
      require(!link.beginFrame((__bridge void *)layer, 120, 75, 9, [] { return true; }),
              "unsupported cap selects timer fallback");
      render();
      require(layer.acquisitions == attachedAcquisitions + 2,
              "timer fallback leaves layer unmanaged");
      std::puts("deterministic native attachment/lease checks passed (callbacks were seeded)");
    } else {
      std::puts("native attachment checks unavailable before macOS 14/iOS 17; provider checks passed");
    }
    // Layout can publish a new layer extent before its resize event reaches
    // the renderer. Both a supplied new drawable and handled-nil must remain
    // safe with old depth/MSAA attachments. The next reconciled frame presents.
    asobmashowMetalSetDrawableProvider(&provider);
    allowPresentation = true;
    for (const auto options : {uint32_t(0), uint32_t(BGFX_RESET_MSAA_X2),
                              uint32_t(BGFX_RESET_MSAA_X2 | BGFX_RESET_SRGB_BACKBUFFER)}) {
      const auto flags = BGFX_RESET_FLIP_AFTER_RENDER | options;
      bgfx::reset(16, 16, flags);
      bgfx::frame();
      bgfx::setViewRect(0, 0, 0, 16, 16);
      bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x123456ff);
      layer.drawableSize = CGSizeMake(32, 8);
      id<CAMetalDrawable> resized = [layer nextDrawable];
      require(resized != nil, "resized layer supplies a drawable");
      supplied = (__bridge void *)resized;
      const auto beforePresent = presentationCalls;
      const auto beforeAcquire = layer.acquisitions;
      bgfx::touch(0);
      bgfx::frame();
      require(presentationCalls == beforePresent && layer.acquisitions == beforeAcquire,
              "new layer extent cannot present against stale renderer attachments");
      supplied = nullptr;
      bgfx::touch(0);
      bgfx::frame();
      require(presentationCalls == beforePresent && layer.acquisitions == beforeAcquire,
              "handled-nil uses compatible fallback attachments without reacquiring");

      bgfx::reset(32, 8, flags);
      bgfx::setViewRect(0, 0, 0, 32, 8);
      bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x123456ff);
      supplied = (__bridge void *)resized;
      bgfx::touch(0);
      bgfx::frame();
      require(presentationCalls == beforePresent + 1 && layer.acquisitions == beforeAcquire,
              "reconciling renderer attachments resumes the supplied drawable");
      supplied = nullptr;
      resized = nil;
    }
    // Secondary swap chains must use their own extent, not the main window's.
    auto *secondaryLayer = [AsoDrawableProbeLayer new];
    const auto secondary = bgfx::createFrameBuffer((__bridge void *)secondaryLayer, 12, 24);
    require(bgfx::isValid(secondary), "secondary Metal swap chain created");
    bgfx::frame();
    id<CAMetalDrawable> secondaryDrawable = [secondaryLayer nextDrawable];
    require(secondaryDrawable != nil, "secondary layer supplies a drawable");
    supplied = (__bridge void *)secondaryDrawable;
    const auto beforeSecondaryPresent = presentationCalls;
    bgfx::setViewFrameBuffer(1, secondary);
    bgfx::setViewRect(1, 0, 0, 12, 24);
    bgfx::setViewClear(1, BGFX_CLEAR_COLOR, 0x123456ff);
    bgfx::touch(1);
    bgfx::frame();
    require(presentationCalls == beforeSecondaryPresent + 1 && secondaryLayer.acquisitions == 1,
            "secondary drawable presents using its own attachments");
    supplied = nullptr;
    secondaryDrawable = nil;
    bgfx::destroy(secondary);
    bgfx::frame();
    asobmashowMetalSetDrawableProvider(nullptr);
    bgfx::shutdown();
    layer = nil;
  }
}
