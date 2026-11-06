#pragma once

#include "RealtimeTouchInputRouter.h"

#include <utility>

namespace gameplay {

// Setup has no scoring worker. Keep the same lane/virtual-controller router
// and skin-control capture rules on the scene thread until an attempt starts.
class PracticeMenuTouchInput {
public:
  PracticeMenuTouchInput(RealtimeTouchInputSink inputSink,
                         RealtimeTouchPresentationSink presentationSink)
      : inputSink_(inputSink), presentation_(presentationSink) {}

  void publish(RealtimeTouchLayout layout, RealtimeTouchHitSnapshot snapshot,
               long long now) {
    if (!router_) {
      router_.emplace(1, std::move(layout), inputSink_);
    } else if (snapshot.layoutRevision != snapshot_.layoutRevision ||
               snapshot.uiTransform != snapshot_.uiTransform) {
      cancel(now);
      (void)router_->updateLayout(std::move(layout), now);
    }
    snapshot_ = std::move(snapshot);
  }

  void consume(std::int64_t finger, RealtimeTouchPhase phase,
               float uiNormalizedX, float uiNormalizedY, long long now) {
    if (!router_) return;
    const auto &transform = snapshot_.uiTransform;
    if (transform.renderWidth <= 0 || transform.renderHeight <= 0) return;
    RealtimeTouchSample sample{
        .fingerId = finger,
        .phase = phase,
        .normalizedX = (uiNormalizedX * transform.uiWidth * transform.uiScaleX +
                         transform.uiOffsetX) / transform.renderWidth,
        .normalizedY = (uiNormalizedY * transform.uiHeight * transform.uiScaleY +
                         transform.uiOffsetY) / transform.renderHeight,
        .steadyTimestampMicros = now};
    populateRealtimeTouchPresentationMetadata(sample, snapshot_, captures_);
    sample.excludedFromGameplay =
        sample.presentationHit.kind != PresentationUiControlKind::None &&
        sample.presentationHit.kind != PresentationUiControlKind::VirtualController;
    auto routed = sample;
    // Setup has no native callback continuation: cancellation must release
    // the first-key hold immediately, including on pause/background/reset.
    if (phase == RealtimeTouchPhase::Cancel) routed.phase = RealtimeTouchPhase::Up;
    (void)router_->consume(routed);
    (void)presentation_.consume(sample, now);
  }

  void advance(long long now) {
    if (router_) (void)router_->advanceSpinScratch(now);
  }

  void cancel(long long now) {
    if (router_) (void)router_->cancelAll(now);
    presentation_.cancelAll(now);
    captures_.reset();
  }

  [[nodiscard]] const RealtimeTouchInputRouter *router() const noexcept {
    return router_ ? &*router_ : nullptr;
  }

private:
  RealtimeTouchInputSink inputSink_;
  std::optional<RealtimeTouchInputRouter> router_;
  RealtimeTouchHitSnapshot snapshot_;
  RealtimeTouchHitCaptureTracker captures_;
  RealtimeTouchPresentationDispatcher presentation_;
};

} // namespace gameplay
