#include "IOSTouchInput.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "AppleInputTimestamp.h"
#include "IOSTouchGestureRouter.h"
#include "../perf/LatencyTelemetry.h"

#import <UIKit/UIKit.h>
#import <UIKit/UIGestureRecognizerSubclass.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

@interface AsoGameplayTouchRecognizer : UIGestureRecognizer <UIGestureRecognizerDelegate>
@end

@implementation AsoGameplayTouchRecognizer {
  input::ios::TouchGestureRouter _router;
  input::TimestampEpochMapping _mapping;
  bool _mappingValid;
  std::vector<input::native_touch::RawTouchEvent> _samples;
}

- (instancetype)init {
  self = [super initWithTarget:nil action:nil];
  if (self) {
    _samples.reserve(128);
    self.delegate = self;
    self.allowedTouchTypes = @[@(UITouchTypeDirect)];
    self.requiresExclusiveTouchType = NO;
    // Recognize immediately on DOWN, before UIKit forwards it into SDL's view.
    self.delaysTouchesBegan = YES;
    self.delaysTouchesEnded = NO;
    self.cancelsTouchesInView = YES;
  }
  return self;
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)recognizer
       shouldReceiveTouch:(UITouch *)touch {
  (void)recognizer;
  // Native text fields and other UIKit child controls retain their own input.
  if (touch.type != UITouchTypeDirect || touch.view != self.view) return NO;
  return _router.claim(static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(touch)));
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)recognizer
    shouldBeRequiredToFailByGestureRecognizer:(UIGestureRecognizer *)other {
  (void)recognizer;
  // SDL's pinch handler also publishes SDL events. It must not run ahead of a
  // gameplay chord, including a two-finger DOWN delivered in one UIKit batch.
  return other.view == self.view && [other isKindOfClass:[UIPinchGestureRecognizer class]] &&
      (_router.contactCount() != 0 ||
       input::native_touch::RawTouchRegistration::activeEpoch() != 0);
}

- (void)dispatchSample:(UITouch *)sample forContact:(UITouch *)contact
                 phase:(input::native_touch::TouchPhase)phase
               mapping:(input::TimestampEpochMapping)mapping {
  const auto bounds = self.view.bounds;
  const auto pointerId = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(contact));
  const auto rejectSample = [&] {
    if (phase == input::native_touch::TouchPhase::Up ||
        phase == input::native_touch::TouchPhase::Cancel) {
      _router.cancel(pointerId, input::apple::steadyNowMicros());
    }
  };
  if (bounds.size.width <= 0 || bounds.size.height <= 0) {
    rejectSample();
    return;
  }
  const CGPoint point = [sample locationInView:self.view];
  const double micros = sample.timestamp * 1000000.0;
  if (!std::isfinite(micros) || micros <= 0 ||
      micros >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()) ||
      !std::isfinite(point.x) || !std::isfinite(point.y)) {
    rejectSample();
    return;
  }
  const auto timestamp = mapping.toSteadyMicros(static_cast<std::uint64_t>(micros));
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
  const auto now = perf::latency::nowMicros();
  if (now >= timestamp) {
    perf::latency::record(perf::latency::Stage::InputDelivery, now - timestamp);
  }
#endif
  _samples.push_back({
      .pointerId = pointerId,
      .phase = phase,
      .x = static_cast<float>((point.x - bounds.origin.x) / bounds.size.width),
      .y = static_cast<float>((point.y - bounds.origin.y) / bounds.size.height),
      .steadyTimestampMicros = timestamp});
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  (void)event;
  if (!_mappingValid) {
    _mapping = input::apple::hostToSteadyEpochMapping();
    _mappingValid = true;
  }
  const auto mapping = _mapping;
  _samples.clear();
  for (UITouch *touch in touches) {
    [self dispatchSample:touch forContact:touch
                   phase:input::native_touch::TouchPhase::Down mapping:mapping];
  }
  _router.dispatchBatch(_samples);
  self.state = self.state == UIGestureRecognizerStatePossible
      ? UIGestureRecognizerStateBegan : UIGestureRecognizerStateChanged;
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  const auto mapping = _mapping;
  _samples.clear();
  for (UITouch *touch in touches) {
    // UIKit returns real samples in time order. Preserve the original contact
    // identity: the history objects are distinct UITouch instances.
    for (UITouch *sample in [event coalescedTouchesForTouch:touch]) {
      [self dispatchSample:sample forContact:touch
                     phase:input::native_touch::TouchPhase::Move mapping:mapping];
    }
    [self dispatchSample:touch forContact:touch
                   phase:input::native_touch::TouchPhase::Move mapping:mapping];
  }
  _router.dispatchBatch(_samples);
  self.state = UIGestureRecognizerStateChanged;
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  const auto mapping = _mapping;
  _samples.clear();
  for (UITouch *touch in touches) {
    for (UITouch *sample in [event coalescedTouchesForTouch:touch]) {
      [self dispatchSample:sample forContact:touch
                     phase:input::native_touch::TouchPhase::Move mapping:mapping];
    }
    [self dispatchSample:touch forContact:touch
                   phase:input::native_touch::TouchPhase::Up mapping:mapping];
  }
  _router.dispatchBatch(_samples);
  self.state = _router.contactCount() == 0
      ? UIGestureRecognizerStateEnded : UIGestureRecognizerStateChanged;
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  (void)event;
  const auto mapping = _mapping;
  _samples.clear();
  for (UITouch *touch in touches) {
    [self dispatchSample:touch forContact:touch
                   phase:input::native_touch::TouchPhase::Cancel mapping:mapping];
  }
  _router.dispatchBatch(_samples);
  self.state = _router.contactCount() == 0
      ? UIGestureRecognizerStateCancelled : UIGestureRecognizerStateChanged;
}

- (void)reset {
  _router.cancelAll(input::apple::steadyNowMicros());
  _mappingValid = false;
  [super reset];
}
@end

namespace {
AsoGameplayTouchRecognizer *gameplayTouchRecognizer;
std::atomic_bool gameplayTouchInstalled{false};
}

bool InstallIOSGameplayTouchInput(void *nativeView) {
  if (![NSThread isMainThread] || nativeView == nullptr) return false;
  UIView *view = (__bridge UIView *)nativeView;
  if (![view isKindOfClass:[UIView class]]) return false;
  UninstallIOSGameplayTouchInput();
  gameplayTouchRecognizer = [[AsoGameplayTouchRecognizer alloc] init];
  [view addGestureRecognizer:gameplayTouchRecognizer];
  gameplayTouchInstalled.store(true, std::memory_order_release);
  return true;
}

void UninstallIOSGameplayTouchInput() {
  gameplayTouchInstalled.store(false, std::memory_order_release);
  [gameplayTouchRecognizer reset];
  [gameplayTouchRecognizer.view removeGestureRecognizer:gameplayTouchRecognizer];
  gameplayTouchRecognizer = nil;
}

void SetIOSGameplayTouchInputEnabled(bool enabled) {
  if (![NSThread isMainThread]) return;
  if (!enabled) [gameplayTouchRecognizer reset];
  gameplayTouchRecognizer.enabled = enabled ? YES : NO;
}

bool IOSGameplayTouchInputInstalled() {
  return gameplayTouchInstalled.load(std::memory_order_acquire);
}

#endif
