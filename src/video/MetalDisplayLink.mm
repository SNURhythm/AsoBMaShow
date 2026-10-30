#include "MetalDisplayLink.h"
#include "MetalDisplayLinkFrame.h"
#include "MetalDrawableProvider.h"

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cassert>
#include <cstdio>

namespace video {
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
  // Temporary, owner-thread diagnostics; no per-frame timing collection.
  bool recreationRequested = false;
  std::uint64_t creationSerial = 0, manualRequests = 0, manualRecreations = 0;
  double createdAt = 0;
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
  state.rate = 0;
  state.recreationRequested = false;
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
                                  std::function<bool()> isCurrent) {
  endFrame();
  auto &state = *state_;
  const float rate = metalDisplayLinkRate(refreshRate, frameCap);
  if (nativeLayer == nullptr || rate == 0 || !isCurrent || !isCurrent()) {
    pause();
    return false;
  }
  if (@available(macOS 14.0, iOS 17.0, *)) {
    @autoreleasepool {
      if (CACurrentMediaTime() < state.retryAfter) return false;
      auto *layer = (__bridge CAMetalLayer *)nativeLayer;
      const bool recreate = state.recreationRequested;
      if (state.layer != layer || recreate) invalidate(state);
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
        ++state.creationSerial;
        state.createdAt = CACurrentMediaTime();
        if (recreate) ++state.manualRecreations;
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
bool MetalDisplayLink::didSchedulePresent() const { return state_->scheduled; }
void MetalDisplayLink::endFrame() { video::endFrame(*state_); }
// Merely setting paused leaves the layer owned by CAMetalDisplayLink, which
// forbids nextDrawable. Detach before timer fallback or export takes ownership.
void MetalDisplayLink::pause() { invalidate(*state_); }
void MetalDisplayLink::pauseCurrent() { if (current) invalidate(*current); }
bool MetalDisplayLink::requestRecreationForDiagnostics() {
  if (current == nullptr || current->link == nil) return false;
  if (!current->recreationRequested) {
    current->recreationRequested = true;
    ++current->manualRequests;
  }
  return true;
}

std::string MetalDisplayLink::diagnosticsForCurrentThread() {
  if (current == nullptr)
    return "APP_METAL_LINK ownerOnCallingThread=0 (native probe or non-owner caller)\n";
  const auto &state = *current;
  const double now = CACurrentMediaTime();
  char report[512];
  std::snprintf(report, sizeof(report),
      "APP_METAL_LINK ownerOnCallingThread=1 attached=%d creationSerial=%llu "
      "linkAgeSeconds=%.3f requestedRate=%.3f preferredFrameLatency=1 "
      "manualRequests=%llu manualRecreations=%llu recreationPending=%d "
      "retryInSeconds=%.3f capturedAtMediaTime=%.3f\n",
      state.link != nil, static_cast<unsigned long long>(state.creationSerial),
      state.link != nil ? now - state.createdAt : 0.0, state.rate,
      static_cast<unsigned long long>(state.manualRequests),
      static_cast<unsigned long long>(state.manualRecreations),
      state.recreationRequested, std::max(0.0, state.retryAfter - now), now);
  return report;
}
} // namespace video
