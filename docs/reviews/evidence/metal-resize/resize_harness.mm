// Source-audit reproducer, not an app or physical-display test.
// See ../../2026-10-30-ios-metal-source-audit.md for commands and limitations.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "video/MetalDisplayLink.mm"
#include <bgfx/bgfx.h>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  @autoreleasepool {
    const bool reconcile = argc > 1 && !std::strcmp(argv[1], "reconcile");
    const bool reject = argc > 1 && !std::strcmp(argv[1], "reject");
    if (@available(macOS 14.0, *)) {} else { return 2; }

    auto *layer = [CAMetalLayer new];
    bgfx::renderFrame();
    bgfx::Init init;
    init.type = bgfx::RendererType::Metal;
    init.fallback = false;
    init.platformData.nwh = (__bridge void *)layer;
    init.resolution.width = 32;
    init.resolution.height = 16;
    init.resolution.reset = BGFX_RESET_FLIP_AFTER_RENDER | BGFX_RESET_MSAA_X2;
    if (!bgfx::init(init)) return 3;
    bgfx::setViewRect(0, 0, 0, 32, 16);
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x102030ff);
    bgfx::touch(0);
    bgfx::frame();

    // Model a UIKit/SDL layer layout that the renderer has not reconciled yet.
    layer.drawableSize = CGSizeMake(16, 32);
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) return 4;
    if (reconcile) {
      bgfx::reset(16, 32, init.resolution.reset);
      bgfx::setViewRect(0, 0, 0, 16, 32);
      bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x102030ff);
      // Execute the reset while detached. The macOS backend also sets the
      // maximum drawable count during resize, which an attached link forbids.
      bgfx::frame();
    }
    {
      video::MetalDisplayLink link;
      auto &state = *video::current;
      state.layer = layer;
      if (@available(macOS 14.0, *)) {
        state.link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
        ((CAMetalDisplayLink *)state.link).paused = YES;
      }
      if (!state.link) return 5;
      // Seed a real drawable acquired BEFORE attachment, like the existing
      // native fixture. There are no real display callbacks in this test.
      state.generation = 1;
      state.isCurrent = [] { return true; };
      if (!reject) state.frame.begin(1);
      const double now = CACurrentMediaTime();
      state.frame.receive(now, now + 1);
      state.drawable = drawable;
      std::fprintf(stderr,
          "AUDIT reconcile=%d reject=%d live=%gx%g drawable=%lux%lu "
          "renderer=%dx%d geometryAccepted=%d leaseReady=%d\n",
          reconcile, reject, layer.drawableSize.width, layer.drawableSize.height,
          (unsigned long)drawable.texture.width, (unsigned long)drawable.texture.height,
          reconcile ? 16 : 32, reconcile ? 32 : 16,
          video::geometryMatches(state, (__bridge void *)layer), state.frame.ready());
      bgfx::touch(0);
      bgfx::frame();
      std::fprintf(stderr, "AUDIT scheduled=%d\n", link.didSchedulePresent());
    }
    drawable = nil;
    bgfx::shutdown();
  }
}
