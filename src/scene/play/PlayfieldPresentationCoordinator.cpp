#include "PlayfieldPresentationCoordinator.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void advanceRevision(std::uint64_t &revision) noexcept {
  revision = revision == std::numeric_limits<std::uint64_t>::max()
                 ? 1
                 : revision + 1;
}

skin::SkinDiagnostic coordinatorDiagnostic(std::string code,
                                           std::string message,
                                           skin::DiagnosticSeverity severity =
                                               skin::DiagnosticSeverity::Error) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = severity};
}

} // namespace

PlayfieldPresentationCoordinator::PlayfieldPresentationCoordinator(
    PlayfieldPresentationCoordinatorDependencies dependencies)
    : builtIn_(std::move(dependencies.builtIn)),
      skin_(std::move(dependencies.skin)), bga_(dependencies.bga),
      persistViewport_(std::move(dependencies.persistViewport)),
      recordFailure_(std::move(dependencies.recordFailure)),
      allowBuiltInFallback_(dependencies.allowBuiltInFallback),
      replayGhostEvents_(std::move(dependencies.replayGhostEvents)) {
  if (!builtIn_) {
    throw std::invalid_argument(
        "PlayfieldPresentationCoordinator requires a built-in presentation");
  }
  observedTouchMode_ = activeMode();
  observedTargetLayoutRevision_ = activeMode() == PresentationMode::Skin
                                      ? skin_->touchLayoutRevision()
                                      : builtIn_->touchLayoutRevision();
  observedTargetHitRevision_ = activeMode() == PresentationMode::Skin
                                   ? skin_->touchHitRegionsRevision()
                                   : builtIn_->touchHitRegionsRevision();
}

PlayfieldPresentationCoordinator::~PlayfieldPresentationCoordinator() {
  cancelAndClearActiveTouches();
}

void PlayfieldPresentationCoordinator::installSkinSession(
    std::unique_ptr<CoordinatedPlaySkinSession> session) {
  cancelAndClearActiveTouches();
  skin_ = std::move(session);
  pending_.reset();
  lastFailure_.reset();
  markTouchTargetChanged();
}

void PlayfieldPresentationCoordinator::clearSkinSession() noexcept {
  cancelAndClearActiveTouches();
  skin_.reset();
  pending_.reset();
  markTouchTargetChanged();
}

bool PlayfieldPresentationCoordinator::resetLayoutToFit() {
  if (!skin_) {
    return false;
  }

  const skin::PlaySkinSessionIdentity identity = skin_->identity();
  const skin::ViewportSettings fit{.mode = skin::ViewportMode::Fit};
  // Viewport replacement invalidates authored hit geometry. Release every
  // coordinator/session capture before applying the new topology.
  cancelAndClearActiveTouches();
  try {
    skin_->setViewport(fit);
  } catch (...) {
    const PresentationFailure failure = makeSkinFailure(
        lastFrameSerial_,
        coordinatorDiagnostic(
            "skin.presentation.viewport_apply_failed",
            "The current gameplay skin could not apply the Fit viewport."));
    publishFailure(failure);
    return false;
  }

  GameplayViewportPersistenceResult persisted;
  try {
    if (persistViewport_) {
      persisted = persistViewport_(identity, fit);
    } else {
      persisted.diagnostic = coordinatorDiagnostic(
          "skin.presentation.viewport_persistence_unavailable",
          "The Fit viewport was applied for this chart, but no persistence "
          "service is available.");
    }
  } catch (...) {
    persisted.diagnostic = coordinatorDiagnostic(
        "skin.presentation.viewport_persistence_failed",
        "The Fit viewport was applied for this chart, but persistence "
        "reported an exception.");
  }

  if (persisted.disposition !=
      GameplayViewportPersistenceDisposition::Queued) {
    skin::SkinDiagnostic diagnostic = persisted.diagnostic.value_or(
        coordinatorDiagnostic(
            persisted.disposition ==
                    GameplayViewportPersistenceDisposition::Deferred
                ? "skin.presentation.viewport_persistence_deferred"
                : "skin.presentation.viewport_persistence_rejected",
            persisted.disposition ==
                    GameplayViewportPersistenceDisposition::Deferred
                ? "The Fit viewport is active and its durable update was "
                  "deferred."
                : "The Fit viewport is active, but its durable update was "
                  "rejected.",
            persisted.disposition ==
                    GameplayViewportPersistenceDisposition::Deferred
                ? skin::DiagnosticSeverity::Warning
                : skin::DiagnosticSeverity::Error));
    const PresentationFailure failure = makeSkinFailure(
        lastFrameSerial_, std::move(diagnostic));
    publishFailure(failure);
  }
  return true;
}

void PlayfieldPresentationCoordinator::updateSkinViewportGeometry(
    skin::UiLogicalRect safeUiBounds) {
  cancelAndClearActiveTouches();
  pending_.reset();
  builtIn_->refreshGeometry();
  if (skin_) {
    skin_->updateViewportGeometry(safeUiBounds);
  }
  markTouchTargetChanged();
}

void PlayfieldPresentationCoordinator::configure(
    const PlayfieldPresentationConfig &configuration) {
  configuration_ = configuration;
  builtIn_->configure(configuration);
}

PresentationFrameOutcome PlayfieldPresentationCoordinator::prepareFrame(
    const PlayfieldVisualState &state,
    const PlayfieldProjectionResult &projection) {
  if (pending_) {
    const PresentationFailure failure = makeSkinFailure(
        state.clock.serial,
        coordinatorDiagnostic(
            "skin.presentation.frame_already_pending",
            "A gameplay presentation frame was prepared before the previous "
            "frame was rendered."));
    publishFailure(failure);
    return PresentationFrameOutcome::CriticalFailure;
  }

  PendingFrame pending;
  pending.frameSerial = state.clock.serial;
  pending.hadSkin = static_cast<bool>(skin_);
  pending.replayGhostFrame = {
      .frameSerial = state.clock.serial,
      .visualTimeMicros = state.clock.visualTimeMicros,
      .currentScrollPosition = projection.currentScrollPosition,
      .hispeed = projection.builtInTraversal
                      ? static_cast<double>(projection.builtInTraversal->hispeed)
                      : 0.0,
      .enabled = configuration_.replayGhostRenderingEnabled &&
                 !replayGhostEvents_.empty(),
      .events = replayGhostEvents_,
  };
  lastFrameSerial_ = state.clock.serial;

  try {
    pending.builtInPrepare = builtIn_->prepareFrame(state, projection);
  } catch (...) {
    pending.builtInPrepare = PresentationFrameOutcome::CriticalFailure;
  }

  // BGA preparation happens after session preparation but is still part of
  // the no-submission boundary, so its exact payload must exist first too.
  pending.bgaPrepareExceptionFailure = makeSkinFailure(
      state.clock.serial,
      coordinatorDiagnostic(
          "skin.presentation.bga_prepare_failed",
          "Gameplay BGA preparation failed before presentation selection."));

  if (skin_) {
    // Own every diagnostic needed by a later no-submission path before
    // entering session preparation/rendering. Moving these payloads through
    // fallback is allocation-free and retains the exact session identity.
    pending.skinPrepareExceptionFailure = makeSkinFailure(
        state.clock.serial,
        coordinatorDiagnostic(
            "skin.presentation.prepare_failed",
            "The gameplay skin reported an exception while preparing its "
            "frame."));
    pending.skinRenderExceptionFailure = makeSkinFailure(
        state.clock.serial,
        coordinatorDiagnostic(
            "skin.presentation.render_failed",
            "The gameplay skin reported an exception before submitting its "
            "frame."));
    pending.skinNoSubmissionFailure = makeSkinFailure(
        state.clock.serial,
        coordinatorDiagnostic(
            "skin.presentation.frame_not_submitted",
            "The gameplay skin did not submit a complete frame."));
    try {
      pending.skinPrepare = skin_->prepareFrame(state, projection);
    } catch (...) {
      pending.skinPrepare = PresentationFrameOutcome::CriticalFailure;
      pending.preparationFailure =
          PendingFrame::PreparationFailure::SkinPrepareException;
    }
  }

  // This is the sole video-update call for this captured frame.  Both the
  // embedded skin path and fullscreen fallback reuse this exact value.
  try {
    pending.bga = bga_.prepareVisualFrameAt(
        state.clock.serial, state.clock.bgaTimeMicros, state.bgaMiss);
  } catch (...) {
    pending.preparationFailure =
        PendingFrame::PreparationFailure::BgaPrepareException;
  }

  const PresentationFrameOutcome outcome =
      pending.preparationFailure != PendingFrame::PreparationFailure::None
          ? PresentationFrameOutcome::CriticalFailure
          : pending.hadSkin ? pending.skinPrepare : pending.builtInPrepare;
  pending_ = std::move(pending);
  return outcome;
}

PresentationFrameResult
PlayfieldPresentationCoordinator::render(RenderContext &context) {
  if (!pending_) {
    PresentationFailure failure = makeSkinFailure(
        0, coordinatorDiagnostic(
               "skin.presentation.frame_not_prepared",
               "The gameplay presentation was rendered without a prepared "
               "frame."));
    publishFailure(failure);
    return {.outcome = PresentationFrameOutcome::CriticalFailure,
            .submittedMode = PresentationMode::BuiltIn,
            .bgaCompositeMode =
                GameplayBgaCompositeMode::FullscreenBuiltIn,
            .failure = std::move(failure)};
  }

  PendingFrame pending = std::move(*pending_);
  pending_.reset();

  const auto abortSelectedSkinFrame =
      [this, &pending](PresentationFailure failure,
                       PresentationFrameOutcome outcome) {
        cancelAndClearActiveTouches();
        skin_.reset();
        markTouchTargetChanged();
        failure.frameSerial = pending.frameSerial;
        publishFailure(failure);
        return PresentationFrameResult{
            .frameSerial = pending.frameSerial,
            .outcome = outcome,
            .submittedMode = PresentationMode::Skin,
            .bgaCompositeMode = GameplayBgaCompositeMode::EmbeddedSkin,
            .preparedBga = std::move(pending.bga),
            .failure = std::move(failure),
        };
      };

  if (!pending.hadSkin) {
    std::optional<PresentationFailure> preparationFailure;
    if (pending.preparationFailure ==
        PendingFrame::PreparationFailure::BgaPrepareException) {
      preparationFailure.emplace(
          std::move(pending.bgaPrepareExceptionFailure));
    }
    const PresentationFrameOutcome selectedOutcome =
        preparationFailure ? PresentationFrameOutcome::CriticalFailure
                           : pending.builtInPrepare;
    auto result = renderBuiltIn(context, std::move(pending),
                                std::move(preparationFailure), selectedOutcome);
    if (result.failure) {
      publishFailure(*result.failure);
    }
    return result;
  }

  if (pending.preparationFailure != PendingFrame::PreparationFailure::None ||
      !pending.bga) {
    std::optional<PresentationFailure> failure;
    if (pending.preparationFailure ==
        PendingFrame::PreparationFailure::SkinPrepareException) {
      failure.emplace(std::move(pending.skinPrepareExceptionFailure));
    } else {
      failure.emplace(std::move(pending.bgaPrepareExceptionFailure));
    }
    if (!allowBuiltInFallback_) {
      return abortSelectedSkinFrame(
          std::move(*failure), PresentationFrameOutcome::CriticalFailure);
    }
    cancelAndClearActiveTouches();
    skin_.reset();
    markTouchTargetChanged();
    auto result = renderBuiltIn(context, std::move(pending), std::move(failure),
                                PresentationFrameOutcome::CriticalFailure);
    if (result.failure) {
      publishFailure(*result.failure);
    }
    return result;
  }

  PresentationFrameResult skinResult;
  bool skinRenderThrew = false;
  try {
    skinResult = skin_->render(context, *pending.bga, bga_);
  } catch (...) {
    skinRenderThrew = true;
  }

  if (skinRenderThrew) {
    std::optional<PresentationFailure> failure;
    failure.emplace(std::move(pending.skinRenderExceptionFailure));
    if (!allowBuiltInFallback_) {
      return abortSelectedSkinFrame(
          std::move(*failure), PresentationFrameOutcome::CriticalFailure);
    }
    cancelAndClearActiveTouches();
    skin_.reset();
    markTouchTargetChanged();
    auto result = renderBuiltIn(context, std::move(pending), std::move(failure),
                                PresentationFrameOutcome::CriticalFailure);
    if (result.failure) {
      publishFailure(*result.failure);
    }
    return result;
  }

  skinResult.frameSerial = pending.frameSerial;
  // A session cannot substitute a separately prepared value: this exact
  // coordinator-owned frame is the only fullscreen/embedded handoff.
  skinResult.preparedBga = pending.bga;
  if (skinResult.failure) {
    skinResult.failure->frameSerial = pending.frameSerial;
  }

  const bool skinSubmitted =
      skinResult.submittedMode == PresentationMode::Skin;
  if (skinSubmitted) {
    // Once skin commands have been submitted, fullscreen or built-in work in
    // the same frame would create a hybrid.  A post-draw recoverable result is
    // therefore reported while the embedded composition remains authoritative.
    skinResult.bgaCompositeMode = GameplayBgaCompositeMode::EmbeddedSkin;
    if (pending.replayGhostFrame.enabled) {
      // This optional application overlay is intentionally post-skin and
      // cannot change the already-submitted selected frame into a fallback.
      try {
        skin_->submitSyntheticReplayGhosts(context, pending.replayGhostFrame);
      } catch (...) {
      }
    }
    if (skinResult.failure) {
      publishFailure(*skinResult.failure);
    }
    if (skinResult.outcome == PresentationFrameOutcome::CriticalFailure) {
      cancelAndClearActiveTouches();
      skin_.reset();
      markTouchTargetChanged();
    }
    return skinResult;
  }

  std::optional<PresentationFailure> failure;
  if (skinResult.failure) {
    failure.emplace(std::move(*skinResult.failure));
  } else {
    failure.emplace(std::move(pending.skinNoSubmissionFailure));
  }
  const PresentationFrameOutcome failureOutcome =
      skinResult.outcome == PresentationFrameOutcome::Ready
          ? PresentationFrameOutcome::CriticalFailure
          : skinResult.outcome;
  if (!allowBuiltInFallback_) {
    return abortSelectedSkinFrame(std::move(*failure), failureOutcome);
  }
  cancelAndClearActiveTouches();
  skin_.reset();
  markTouchTargetChanged();
  auto result = renderBuiltIn(context, std::move(pending), std::move(failure),
                              failureOutcome);
  if (result.failure) {
    publishFailure(*result.failure);
  }
  return result;
}

PresentationFrameResult PlayfieldPresentationCoordinator::renderBuiltIn(
    RenderContext &context, PendingFrame pending,
    std::optional<PresentationFailure> selectedFailure,
    PresentationFrameOutcome selectedOutcome) {
  PresentationFrameResult result;
  try {
    result = builtIn_->render(context);
  } catch (...) {
    result.outcome = PresentationFrameOutcome::CriticalFailure;
  }
  result.frameSerial = pending.frameSerial;
  result.submittedMode = PresentationMode::BuiltIn;
  result.bgaCompositeMode = GameplayBgaCompositeMode::FullscreenBuiltIn;
  result.preparedBga = std::move(pending.bga);
  if (selectedFailure) {
    result.failure = std::move(selectedFailure);
    result.outcome = selectedOutcome;
  } else if (result.failure) {
    publishFailure(*result.failure);
  }
  return result;
}

gameplay::RealtimeTouchLayout
PlayfieldPresentationCoordinator::touchLayout() const {
  synchronizeTouchRevisions();
  gameplay::RealtimeTouchLayout layout =
      activeMode() == PresentationMode::Skin ? skin_->touchLayout()
                                              : builtIn_->touchLayout();
  layout.revision = publishedLayoutRevision_;
  return layout;
}

std::uint64_t
PlayfieldPresentationCoordinator::touchLayoutRevision() const noexcept {
  synchronizeTouchRevisions();
  return publishedLayoutRevision_;
}

std::uint64_t
PlayfieldPresentationCoordinator::touchHitRegionsRevision() const noexcept {
  synchronizeTouchRevisions();
  return publishedHitRevision_;
}

std::vector<PresentationUiHitRegion>
PlayfieldPresentationCoordinator::touchHitRegions() const {
  synchronizeTouchRevisions();
  std::vector<PresentationUiHitRegion> regions =
      activeMode() == PresentationMode::Skin ? skin_->touchHitRegions()
                                              : builtIn_->touchHitRegions();
  for (auto &region : regions) {
    if (region.hit.kind != PresentationUiControlKind::None) {
      region.hit.layoutRevision = publishedLayoutRevision_;
    }
  }
  return regions;
}

PresentationUiHit PlayfieldPresentationCoordinator::hitTestUiControl(
    UiLogicalPoint point) const {
  synchronizeTouchRevisions();
  PresentationUiHit hit = activeMode() == PresentationMode::Skin
                              ? skin_->hitTestUiControl(point)
                              : builtIn_->hitTestUiControl(point);
  if (hit.kind != PresentationUiControlKind::None) {
    hit.layoutRevision = publishedLayoutRevision_;
  }
  return hit;
}

PresentationTouchResult
PlayfieldPresentationCoordinator::beginPresentationTouch(
    const PresentationTouchEvent &event) {
  lastEventMicros_ = event.eventMicros;
  synchronizeTouchRevisions();
  if (event.hit.kind == PresentationUiControlKind::None ||
      event.hit.layoutRevision != publishedLayoutRevision_ ||
      findTouchCapture(event.pointerId) != nullptr) {
    return {};
  }
  const PresentationUiHit expected = hitTestUiControl(event.uiPoint);
  if (expected.kind == PresentationUiControlKind::None ||
      expected != event.hit) {
    return {};
  }
  TouchCapture *capture = allocateTouchCapture(event.pointerId);
  if (capture == nullptr) {
    return {};
  }
  const auto translated = translateTouchEventForActiveTarget(event);
  PresentationTouchResult result;
  try {
    result = activeMode() == PresentationMode::Skin
                 ? skin_->beginPresentationTouch(translated)
                 : builtIn_->beginPresentationTouch(translated);
  } catch (...) {
    return {};
  }
  if (result.consumed) {
    *capture = {.pointerId = event.pointerId,
                .targetGeneration = touchTargetGeneration_,
                .active = true,
                .publicHit = expected};
  }
  return result;
}

PresentationTouchResult
PlayfieldPresentationCoordinator::updatePresentationTouch(
    const PresentationTouchEvent &event) {
  lastEventMicros_ = event.eventMicros;
  TouchCapture *capture = findTouchCapture(event.pointerId);
  if (capture == nullptr ||
      capture->targetGeneration != touchTargetGeneration_) {
    return {};
  }
  PresentationTouchEvent capturedEvent = event;
  capturedEvent.hit = capture->publicHit;
  const auto translated = translateTouchEventForActiveTarget(capturedEvent);
  try {
    return activeMode() == PresentationMode::Skin
               ? skin_->updatePresentationTouch(translated)
               : builtIn_->updatePresentationTouch(translated);
  } catch (...) {
    return {};
  }
}

PresentationTouchResult
PlayfieldPresentationCoordinator::endPresentationTouch(
    const PresentationTouchEvent &event, bool cancelled) {
  lastEventMicros_ = event.eventMicros;
  TouchCapture *stored = findTouchCapture(event.pointerId);
  if (stored == nullptr ||
      stored->targetGeneration != touchTargetGeneration_) {
    return {};
  }
  const TouchCapture capture = *stored;
  *stored = {};
  PresentationTouchEvent capturedEvent = event;
  capturedEvent.hit = capture.publicHit;
  const auto translated = translateTouchEventForActiveTarget(capturedEvent);
  try {
    return activeMode() == PresentationMode::Skin
               ? skin_->endPresentationTouch(translated, cancelled)
               : builtIn_->endPresentationTouch(translated, cancelled);
  } catch (...) {
    return {};
  }
}

void PlayfieldPresentationCoordinator::cancelPresentationTouches(
    long long eventMicros) {
  lastEventMicros_ = eventMicros;
  cancelAndClearActiveTouches();
}

void PlayfieldPresentationCoordinator::onLanePressed(
    int lane, JudgeResult judge, long long eventMicros) {
  lastEventMicros_ = eventMicros;
  try {
    builtIn_->onLanePressed(lane, judge, eventMicros);
  } catch (...) {
  }
  if (skin_) {
    try {
      skin_->onLanePressed(lane, judge, eventMicros);
    } catch (...) {
    }
  }
}

void PlayfieldPresentationCoordinator::onLaneReleased(int lane,
                                                       long long eventMicros) {
  lastEventMicros_ = eventMicros;
  try {
    builtIn_->onLaneReleased(lane, eventMicros);
  } catch (...) {
  }
  if (skin_) {
    try {
      skin_->onLaneReleased(lane, eventMicros);
    } catch (...) {
    }
  }
}

void PlayfieldPresentationCoordinator::onJudge(
    JudgeResult judge, int combo, int score, PlayfieldJudgeEventClock clock,
    bool recordTimingSample) {
  lastEventMicros_ = clock.visualTimeMicros;
  try {
    builtIn_->onJudge(judge, combo, score, clock, recordTimingSample);
  } catch (...) {
  }
  if (skin_) {
    try {
      skin_->onJudge(judge, combo, score, clock, recordTimingSample);
    } catch (...) {
    }
  }
}

void PlayfieldPresentationCoordinator::reset() {
  cancelAndClearActiveTouches();
  skin_.reset();
  pending_.reset();
  builtIn_->reset();
  lastFailure_.reset();
  lastFrameSerial_ = 0;
  markTouchTargetChanged();
}

void PlayfieldPresentationCoordinator::refreshGeometry() {
  builtIn_->refreshGeometry();
  synchronizeTouchRevisions();
}

PresentationMode
PlayfieldPresentationCoordinator::activeMode() const noexcept {
  return skin_ ? PresentationMode::Skin : PresentationMode::BuiltIn;
}

std::optional<PresentationFailure>
PlayfieldPresentationCoordinator::lastFailure() const {
  return lastFailure_;
}

PresentationFailure PlayfieldPresentationCoordinator::makeSkinFailure(
    std::uint64_t frameSerial, skin::SkinDiagnostic diagnostic) const {
  PresentationFailure failure{.diagnostic = std::move(diagnostic),
                              .frameSerial = frameSerial};
  if (skin_) {
    const auto &identity = skin_->identity();
    failure.entry = identity.entry;
    failure.revisionDigest = identity.revisionDigest;
    failure.configurationDigest = identity.configurationDigest;
  }
  return failure;
}

void PlayfieldPresentationCoordinator::publishFailure(
    const PresentationFailure &failure) noexcept {
  try {
    lastFailure_ = failure;
  } catch (...) {
    // Failure reporting may allocate; presentation selection must remain
    // deterministic even if retaining the diagnostic is impossible.
  }
  if (recordFailure_) {
    try {
      recordFailure_(failure);
    } catch (...) {
      // Diagnostics are observational and cannot invalidate an already
      // selected/submitted presentation.
    }
  }
}

void PlayfieldPresentationCoordinator::cancelAndClearActiveTouches() noexcept {
  try {
    if (skin_) {
      skin_->cancelPresentationTouches(lastEventMicros_);
    } else {
      builtIn_->cancelPresentationTouches(lastEventMicros_);
    }
  } catch (...) {
  }
  clearTouchCaptures();
}

void PlayfieldPresentationCoordinator::clearTouchCaptures() noexcept {
  touchCaptures_.fill({});
}

void PlayfieldPresentationCoordinator::markTouchTargetChanged() noexcept {
  advanceRevision(touchTargetGeneration_);
  advanceRevision(publishedLayoutRevision_);
  advanceRevision(publishedHitRevision_);
  observedTouchMode_ = activeMode();
  observedTargetLayoutRevision_ =
      activeMode() == PresentationMode::Skin ? skin_->touchLayoutRevision()
                                              : builtIn_->touchLayoutRevision();
  observedTargetHitRevision_ =
      activeMode() == PresentationMode::Skin
          ? skin_->touchHitRegionsRevision()
          : builtIn_->touchHitRegionsRevision();
}

void PlayfieldPresentationCoordinator::synchronizeTouchRevisions() const
    noexcept {
  const PresentationMode mode = activeMode();
  const std::uint64_t layoutRevision =
      mode == PresentationMode::Skin ? skin_->touchLayoutRevision()
                                     : builtIn_->touchLayoutRevision();
  const std::uint64_t hitRevision =
      mode == PresentationMode::Skin ? skin_->touchHitRegionsRevision()
                                     : builtIn_->touchHitRegionsRevision();
  if (mode != observedTouchMode_) {
    advanceRevision(publishedLayoutRevision_);
    advanceRevision(publishedHitRevision_);
  } else if (layoutRevision != observedTargetLayoutRevision_) {
    advanceRevision(publishedLayoutRevision_);
    advanceRevision(publishedHitRevision_);
  } else if (hitRevision != observedTargetHitRevision_) {
    advanceRevision(publishedHitRevision_);
  }
  observedTouchMode_ = mode;
  observedTargetLayoutRevision_ = layoutRevision;
  observedTargetHitRevision_ = hitRevision;
}

PlayfieldPresentationCoordinator::TouchCapture *
PlayfieldPresentationCoordinator::findTouchCapture(
    long long pointerId) noexcept {
  for (auto &capture : touchCaptures_) {
    if (capture.active && capture.pointerId == pointerId) {
      return &capture;
    }
  }
  return nullptr;
}

PlayfieldPresentationCoordinator::TouchCapture *
PlayfieldPresentationCoordinator::allocateTouchCapture(
    long long pointerId) noexcept {
  if (findTouchCapture(pointerId) != nullptr) {
    return nullptr;
  }
  for (auto &capture : touchCaptures_) {
    if (!capture.active) {
      return &capture;
    }
  }
  return nullptr;
}

PresentationTouchEvent
PlayfieldPresentationCoordinator::translateTouchEventForActiveTarget(
    const PresentationTouchEvent &event) const {
  PresentationTouchEvent translated = event;
  if (translated.hit.kind != PresentationUiControlKind::None) {
    translated.hit.layoutRevision =
        activeMode() == PresentationMode::Skin
            ? skin_->touchLayoutRevision()
            : builtIn_->touchLayoutRevision();
  }
  return translated;
}
