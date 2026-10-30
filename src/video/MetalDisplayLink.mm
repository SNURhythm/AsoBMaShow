#include "MetalDisplayLink.h"
#include "MetalDisplayLinkFrame.h"
#include "MetalDrawableProvider.h"

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <TargetConditionals.h>

namespace video {
struct MetalDisplayLinkWarmup {
  enum Phase { NeedsFrame, WaitingForDisplay, Displayed };
  std::atomic<Phase> phase{NeedsFrame};
  double requestedAt = 0; // Render owner only; completion writes only phase.
};
struct MetalDisplayLinkState {
  // All state, callbacks and run-loop operations belong to one worker.
  CAMetalLayer *layer = nil;
  id link = nil;
  NSObject *delegate = nil;
  id<CAMetalDrawable> drawable = nil;
  MetalDisplayLinkFrame frame;
  std::function<bool()> isCurrent;
  std::function<void(bool)> attachmentChanged;
  bool notifiedAttached = false;
  std::uint64_t generation = 0;
  float rate = 0;
  double retryAfter = 0;
  bool claimed = false, scheduled = false;
  std::shared_ptr<MetalDisplayLinkWarmup> warmup;
  CGSize drawableSize = CGSizeZero;
  CGRect bounds = CGRectZero;
  MTLPixelFormat pixelFormat = MTLPixelFormatInvalid;
  std::uint64_t geometryGeneration = 0;
};
}

API_AVAILABLE(macos(14.0), ios(17.0))
@interface AsoMetalDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate> {
@public
  video::MetalDisplayLinkState *state;
}
@end

@implementation AsoMetalDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link
            needsUpdate:(CAMetalDisplayLinkUpdate *)update {
  if (state->link == link && state->isCurrent && state->isCurrent() &&
      update.drawable != nil &&
      state->frame.receive(CACurrentMediaTime(), update.targetTimestamp)) {
    state->drawable = update.drawable;
    CFRunLoopStop(CFRunLoopGetCurrent());
  }
}
@end

namespace video {
namespace {
thread_local MetalDisplayLinkState *current = nullptr;

void endFrame(MetalDisplayLinkState &state) {
  state.frame.finish();
  state.drawable = nil;
  state.isCurrent = {};
  state.claimed = false;
  state.scheduled = false;
}
void pause(MetalDisplayLinkState &state) {
  endFrame(state);
  if (@available(macOS 14.0, iOS 17.0, *))
    ((CAMetalDisplayLink *)state.link).paused = YES;
}
void invalidate(MetalDisplayLinkState &state) {
  pause(state);
  if (@available(macOS 14.0, iOS 17.0, *)) {
    [(CAMetalDisplayLink *)state.link invalidate];
    ((CAMetalDisplayLink *)state.link).delegate = nil;
  }
  state.link = nil;
  state.delegate = nil;
  state.layer = nil;
  state.warmup.reset();
  state.rate = 0;
  if (state.notifiedAttached) {
    state.notifiedAttached = false;
    if (state.attachmentChanged) state.attachmentChanged(false);
  }
}
bool geometryMatches(const MetalDisplayLinkState &state, void *layer) {
  if (layer != (__bridge void *)state.layer || state.drawable == nil ||
      state.drawable.layer != state.layer) return false;
  const auto texture = state.drawable.texture;
  const auto size = state.layer.drawableSize;
  return texture.width == size.width && texture.height == size.height &&
         texture.pixelFormat == state.layer.pixelFormat;
}
bool acquireDrawable(void *layer, void **drawable) {
  // The attachment owns the layer even between leases and after rejection.
  // Returning false here would illegally call nextDrawable on that layer.
  if (current == nullptr || current->link == nil ||
      layer != (__bridge void *)current->layer) return false;
  auto &state = *current;
  const bool claimed = state.frame.claim(state.isCurrent && state.isCurrent(),
                                         state.generation, geometryMatches(state, layer));
  state.claimed = state.claimed || claimed;
  *drawable = claimed ? (__bridge void *)state.drawable : nullptr;
  return true;
}
bool shouldPresent(void *layer, void *drawable) {
  // Observe only the first ordinary presentation of this surface epoch. A GPU
  // completion or a submitted present does not mean the frame reached display.
  // The handler owns a tiny token, never the layer or the renderer's state.
  if (current != nullptr && current->link == nil && current->warmup &&
      layer == (__bridge void *)current->layer &&
      current->isCurrent && current->isCurrent()) {
#if !TARGET_OS_SIMULATOR
    auto nativeDrawable = (__bridge id<CAMetalDrawable>)drawable;
    auto warmup = current->warmup;
    auto expected = MetalDisplayLinkWarmup::NeedsFrame;
    if (nativeDrawable.layer == current->layer &&
        nativeDrawable.texture.width == current->drawableSize.width &&
        nativeDrawable.texture.height == current->drawableSize.height &&
        nativeDrawable.texture.pixelFormat == current->pixelFormat &&
        warmup->phase.compare_exchange_strong(expected, MetalDisplayLinkWarmup::WaitingForDisplay)) {
      warmup->requestedAt = CACurrentMediaTime();
      [nativeDrawable addPresentedHandler:^(id<MTLDrawable> presented) {
        warmup->phase.store(presented.presentedTime > 0
            ? MetalDisplayLinkWarmup::Displayed : MetalDisplayLinkWarmup::NeedsFrame,
            std::memory_order_release);
      }];
    }
#endif
  }
  if (current == nullptr || current->link == nil ||
      layer != (__bridge void *)current->layer) return true;
  auto &state = *current;
  const bool allowed = state.claimed && !state.scheduled &&
      drawable == (__bridge void *)state.drawable && geometryMatches(state, layer) &&
      state.isCurrent && state.isCurrent();
  state.scheduled = state.scheduled || allowed;
  return allowed;
}
const MetalDrawableProvider provider{acquireDrawable, shouldPresent};
}

MetalDisplayLink::MetalDisplayLink(std::function<void(bool)> attachmentChanged)
    : state_(std::make_unique<MetalDisplayLinkState>()) {
  assert(current == nullptr);
  state_->attachmentChanged = std::move(attachmentChanged);
  current = state_.get();
  asobmashowMetalSetDrawableProvider(&provider);
}
MetalDisplayLink::~MetalDisplayLink() {
  invalidate(*state_);
  asobmashowMetalSetDrawableProvider(nullptr);
  current = nullptr;
}

bool MetalDisplayLink::beginFrame(void *nativeLayer, float refreshRate,
                                  std::uint32_t frameCap, std::uint64_t generation,
                                  std::function<bool()> isCurrent,
                                  std::uint64_t geometryGeneration, bool geometryChanging) {
  endFrame();
  auto &state = *state_;
  const float rate = metalDisplayLinkRate(refreshRate, frameCap);
  if (nativeLayer == nullptr || rate == 0 || geometryChanging || !isCurrent || !isCurrent()) {
    pause();
    return false;
  }
#if TARGET_OS_SIMULATOR
  // Simulator has no displayed-drawable callback to establish this handoff.
  pause();
  return false;
#endif
  if (@available(macOS 14.0, iOS 17.0, *)) {
    @autoreleasepool {
      if (CACurrentMediaTime() < state.retryAfter) return false;
      auto *layer = (__bridge CAMetalLayer *)nativeLayer;
      if (state.layer != layer || state.generation != generation ||
          state.geometryGeneration != geometryGeneration ||
          !CGSizeEqualToSize(state.drawableSize, layer.drawableSize) ||
          !CGRectEqualToRect(state.bounds, layer.bounds) ||
          state.pixelFormat != layer.pixelFormat) invalidate(state);
      if (!state.warmup) {
        state.layer = layer;
        state.generation = generation;
        state.geometryGeneration = geometryGeneration;
        state.drawableSize = layer.drawableSize;
        state.bounds = layer.bounds;
        state.pixelFormat = layer.pixelFormat;
        state.warmup = std::make_shared<MetalDisplayLinkWarmup>();
      }
      if (state.warmup->phase.load(std::memory_order_acquire) ==
              MetalDisplayLinkWarmup::WaitingForDisplay &&
          CACurrentMediaTime() - state.warmup->requestedAt >= 1.0) {
        // A missing observer must not strand this surface in fallback. Retire
        // its token so a late completion cannot authorize the replacement.
        state.warmup = std::make_shared<MetalDisplayLinkWarmup>();
      }
      if (state.warmup->phase.load(std::memory_order_acquire) != MetalDisplayLinkWarmup::Displayed) {
        state.isCurrent = std::move(isCurrent);
        return false;
      }
      if (state.link == nil) {
        state.layer = layer;
        auto *delegate = [AsoMetalDisplayLinkDelegate new];
        delegate->state = &state;
        state.delegate = delegate;
        auto *link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
        state.link = link;
        if (link == nil) {
          invalidate(state);
          state.retryAfter = CACurrentMediaTime() + 1.0;
          return false;
        }
        link.delegate = delegate;
        link.preferredFrameLatency = 1.0F;
        link.paused = YES;
        [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
      }
      auto *link = (CAMetalDisplayLink *)state.link;
      if (state.rate != rate) {
        link.preferredFrameRateRange = CAFrameRateRangeMake(rate, rate, rate);
        state.rate = rate;
      }
      if (!state.notifiedAttached) {
        state.notifiedAttached = true;
        if (state.attachmentChanged) state.attachmentChanged(true);
      }
      state.generation = generation;
      state.isCurrent = std::move(isCurrent);
      state.frame.begin(generation);
      link.paused = NO;
      const double timeout = CACurrentMediaTime() + std::max(0.05, 3.0 / rate);
      while (!state.frame.ready() && state.isCurrent && state.isCurrent()) {
        const double remaining = timeout - CACurrentMediaTime();
        if (remaining <= 0) break;
        // Short slices bound lifecycle/quit response even if callbacks stop.
        const auto result = CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                                               std::min(0.004, remaining), true);
        if (result == kCFRunLoopRunFinished) break;
      }
      if (state.frame.ready() && state.isCurrent && state.isCurrent()) return true;
      const bool missingCallback = state.isCurrent && state.isCurrent();
      pause();
      if (missingCallback) state.retryAfter = CACurrentMediaTime() + 1.0;
    }
  }
  return false;
}

bool MetalDisplayLink::hasFrame() const { return state_->frame.managed(); }
bool MetalDisplayLink::needsOrdinaryFrame() const {
  return state_->warmup && state_->warmup->phase.load(std::memory_order_acquire) ==
      MetalDisplayLinkWarmup::NeedsFrame;
}
bool MetalDisplayLink::didSchedulePresent() const { return state_->scheduled; }
void MetalDisplayLink::endFrame() { video::endFrame(*state_); }
// Merely setting paused leaves the layer owned by CAMetalDisplayLink, which
// forbids nextDrawable. Detach before timer fallback or export takes ownership.
void MetalDisplayLink::pause() { invalidate(*state_); }
void MetalDisplayLink::pauseCurrent() { if (current) invalidate(*current); }
} // namespace video
