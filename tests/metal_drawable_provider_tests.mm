#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include "video/MetalDrawableProvider.h"
#include "video/MetalDisplayLink.h"
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
    auto *layer = [AsoDrawableProbeLayer new];
    bgfx::renderFrame();
    bgfx::Init init;
    init.type = bgfx::RendererType::Metal;
    init.fallback = false;
    init.platformData.nwh = layer;
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

    id<CAMetalDrawable> drawable = [[layer nextDrawable] retain];
    require(drawable != nil, "native layer supplies a drawable for handoff");
    supplied = drawable;
    const auto acquired = layer.acquisitions;
    allowPresentation = false;
    bgfx::touch(0);
    bgfx::frame();
    require(providerCalls == 2 && presentationCalls == 1 &&
                layer.acquisitions == acquired,
            "borrowed drawable is checked at presentation without reacquisition");
    supplied = nullptr;
    [drawable release];

    asobmashowMetalSetDrawableProvider(nullptr);
    bgfx::touch(0);
    bgfx::frame();
    require(layer.acquisitions == acquired + 1,
            "removing provider restores native drawable acquisition");
    {
      video::MetalDisplayLink link;
      link.beginFrame(layer, 60, 0, 1, [] { return true; });
      link.pause();
      require([layer nextDrawable] != nil,
              "pausing scene pacing returns the layer to timer/export acquisition");
    }
    bgfx::shutdown();
    [layer release];
  }
}
