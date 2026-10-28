#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include "video/MetalDrawableProvider.h"
#include "../src/video/MetalDisplayLink.mm"
#include <bgfx/bgfx.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

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
std::uint64_t activeTimingToken = 0;
std::uint64_t timingToken() { return activeTimingToken; }
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
const video::MetalDrawableProvider provider{acquire, present, timingToken};
}

int main() {
  @autoreleasepool {
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
    using namespace video::presentationTiming;
    activeTimingToken = capture().begin(CACurrentMediaTime(), 0, 0, 41);
    const auto rejectedToken = activeTimingToken;
    bgfx::touch(0);
    bgfx::frame();
    require(providerCalls == 2 && presentationCalls == 1 &&
                layer.acquisitions == acquired,
            "borrowed drawable is checked at presentation without reacquisition");
    allowPresentation = true;
    activeTimingToken = capture().begin(CACurrentMediaTime(),
        CACurrentMediaTime() + .008, CACurrentMediaTime() + .016, 42);
    bgfx::touch(0);
    bgfx::frame();
    require(providerCalls == 3 && presentationCalls == 2 && layer.acquisitions == acquired,
            "valid borrowed provider drawable uses ordinary command-buffer presentation");
    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool timingComplete = false;
    do {
      const auto rows = capture().snapshot();
      unsigned recorded = 0;
      for (const auto &row : rows) {
        if (row.token != rejectedToken && row.token != activeTimingToken) continue;
        for (auto event : {Event::RenderSubmit, Event::GpuStart, Event::GpuEnd,
                           Event::PresentRequest, Event::PresentScheduled})
          recorded += row.times[static_cast<unsigned>(event)] > 0;
        const auto start = row.times[static_cast<unsigned>(Event::GpuStart)];
        const auto end = row.times[static_cast<unsigned>(Event::GpuEnd)];
        require(!start || !end || end >= start, "native GPU times are ordered");
      }
      // Three render stages on both frames, two presentation stages only on
      // the accepted frame. Snapshot contention is an explicit capture loss.
      timingComplete = recorded + capture().droppedUpdates() == 8;
      if (!timingComplete) std::this_thread::yield();
    } while (!timingComplete && std::chrono::steady_clock::now() < timeout);
    require(timingComplete,
            "real Metal callbacks record the provider token or report contention loss");
    for (const auto &row : capture().snapshot()) {
      if (row.token == rejectedToken) {
        require(row.times[static_cast<unsigned>(Event::PresentRequest)] == 0 &&
                row.times[static_cast<unsigned>(Event::PresentScheduled)] == 0 &&
                row.times[static_cast<unsigned>(Event::Displayed)] == 0,
                "rejected drawable cannot manufacture presentation timestamps");
      }
    }
    activeTimingToken = 0;
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
      const auto cpuToken = capture().begin(CACurrentMediaTime(), 0, 0, 7);
      state.timingToken = cpuToken;
      video::frameCpuTiming::beginFrame(cpuToken, +[]() noexcept { return CACurrentMediaTime(); });
      { video::frameCpuTiming::Scope cpuSubmit(video::frameCpuTiming::Stage::FrameSubmit); }
      link.endFrame();
      bool foundCpu = false;
      for (const auto &row : capture().snapshot()) {
        if (row.token != cpuToken) continue;
        foundCpu = row.cpu.collected &&
            row.cpu.stages[static_cast<unsigned>(video::frameCpuTiming::Stage::FrameSubmit)].count == 1 &&
            row.times[static_cast<unsigned>(Event::FrameEnd)] > 0;
      }
      require(foundCpu && !video::frameCpuTiming::active(),
              "native endFrame publishes and clears CPU timing under the lease token");
      render();
      require(layer.acquisitions == attachedAcquisitions && !link.didSchedulePresent(),
              "endFrame must preserve attached layer ownership");
      state.generation = 8;
      state.isCurrent = [&] { return active; };
      state.frame.begin(8);
      state.frame.receive(11.0, 11.008);
      state.drawable = leased;
      active = false;
      render();
      require(layer.acquisitions == attachedAcquisitions && !link.didSchedulePresent(),
              "lifecycle rejection must not fall through to nextDrawable");
      link.pause();
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
    bgfx::shutdown();
    layer = nil;
  }
}
