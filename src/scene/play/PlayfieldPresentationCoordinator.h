#pragma once

#include "CoordinatedPlaySkinSession.h"
#include "PlayfieldPresentation.h"
#include "PlayfieldProjection.h"

#include "../../ReplayGhostUtils.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

enum class GameplayViewportPersistenceDisposition : std::uint8_t {
  Queued,
  Deferred,
  Rejected,
};

struct GameplayViewportPersistenceResult {
  GameplayViewportPersistenceDisposition disposition =
      GameplayViewportPersistenceDisposition::Rejected;
  std::optional<skin::SkinDiagnostic> diagnostic;
};

using PersistGameplayViewport = std::function<GameplayViewportPersistenceResult(
    const skin::PlaySkinSessionIdentity &, skin::ViewportSettings)>;

struct PlayfieldPresentationCoordinatorDependencies {
  std::unique_ptr<PlayfieldPresentation> builtIn;
  std::unique_ptr<CoordinatedPlaySkinSession> skin;
  IGameplayBgaSubmitter &bga;
  PersistGameplayViewport persistViewport;
  std::function<void(const PresentationFailure &)> recordFailure;
  std::vector<ReplayGhostEvent> replayGhostEvents;
};

// Owns the built-in presentation and the optional selected skin for one chart.
// A selected skin frame either submits as skin or reports its error; it is
// never silently replaced by built-in gameplay.
class PlayfieldPresentationCoordinator final : public PlayfieldPresentation {
public:
  explicit PlayfieldPresentationCoordinator(
      PlayfieldPresentationCoordinatorDependencies);
  ~PlayfieldPresentationCoordinator() override;

  PlayfieldPresentationCoordinator(const PlayfieldPresentationCoordinator &) =
      delete;
  PlayfieldPresentationCoordinator &
  operator=(const PlayfieldPresentationCoordinator &) = delete;

  void installSkinSession(std::unique_ptr<CoordinatedPlaySkinSession>);
  void clearSkinSession() noexcept;
  [[nodiscard]] bool resetLayoutToFit();
  // Rotation/safe-area replacement is distinct from a user viewport choice.
  // The caller suppresses unchanged rectangles; this operation invalidates
  // pending input/frame geometry for both presentation targets.
  void updateSkinViewportGeometry(skin::UiLogicalRect safeUiBounds);

  void configure(const PlayfieldPresentationConfig &) override;
  [[nodiscard]] PresentationFrameOutcome
  prepareFrame(const PlayfieldVisualState &,
               const PlayfieldProjectionResult &) override;
  [[nodiscard]] PresentationFrameResult render(RenderContext &) override;
  [[nodiscard]] gameplay::RealtimeTouchLayout touchLayout() const override;
  [[nodiscard]] std::uint64_t touchLayoutRevision() const noexcept override;
  [[nodiscard]] std::uint64_t touchHitRegionsRevision() const noexcept override;
  [[nodiscard]] std::vector<PresentationUiHitRegion>
  touchHitRegions() const override;
  [[nodiscard]] PresentationUiHit
      hitTestUiControl(UiLogicalPoint) const override;
  PresentationTouchResult
  beginPresentationTouch(const PresentationTouchEvent &) override;
  PresentationTouchResult
  updatePresentationTouch(const PresentationTouchEvent &) override;
  PresentationTouchResult endPresentationTouch(const PresentationTouchEvent &,
                                               bool cancelled) override;
  void cancelPresentationTouches(long long eventMicros) override;
  void onLanePressed(int, JudgeResult, long long) override;
  void onLaneReleased(int, long long) override;
  void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock, bool) override;
  void reset() override;
  void refreshGeometry() override;
  [[nodiscard]] PresentationMode activeMode() const noexcept override;
  [[nodiscard]] std::optional<PresentationFailure> lastFailure() const override;
  [[nodiscard]] std::optional<skin::SkinGameplayTiming>
  selectedSkinGameplayTiming() const override;

private:
  struct PendingFrame {
    enum class PreparationFailure : std::uint8_t {
      None,
      SkinPrepareException,
      BgaPrepareException,
    };

    std::uint64_t frameSerial = 0;
    PresentationFrameOutcome builtInPrepare =
        PresentationFrameOutcome::CriticalFailure;
    PresentationFrameOutcome skinPrepare =
        PresentationFrameOutcome::CriticalFailure;
    bool hadSkin = false;
    std::optional<std::uint32_t> nextRetainedTimelineOrdinal;
    std::optional<PreparedGameplayBgaFrame> bga;
    PreparationFailure preparationFailure = PreparationFailure::None;
    // Constructed before any fallible skin work. Once evaluation begins,
    // every no-submission decision can return one exact-identity payload
    // without allocating.
    PresentationFailure skinPrepareExceptionFailure;
    PresentationFailure bgaPrepareExceptionFailure;
    PresentationFailure skinRenderExceptionFailure;
    PresentationFailure skinNoSubmissionFailure;
    skin::SyntheticReplayGhostFrameInput replayGhostFrame;
    std::vector<int> startLaneIndicatorLanes;
    double startLaneIndicatorVisibleLaneHeightRatio = 1.0;
    bool startLaneIndicatorsVisible = false;
  };
  struct TouchCapture {
    long long pointerId = 0;
    std::uint64_t targetGeneration = 0;
    bool active = false;
    PresentationUiHit publicHit;
  };

  [[nodiscard]] PresentationFrameResult renderBuiltIn(RenderContext &,
                                                      PendingFrame);
  [[nodiscard]] PresentationFailure makeSkinFailure(std::uint64_t frameSerial,
                                                    skin::SkinDiagnostic) const;
  void publishFailure(const PresentationFailure &) noexcept;
  void cancelAndClearActiveTouches() noexcept;
  void clearTouchCaptures() noexcept;
  void markTouchTargetChanged() noexcept;
  void synchronizeTouchRevisions() const noexcept;
  [[nodiscard]] TouchCapture *findTouchCapture(long long) noexcept;
  [[nodiscard]] TouchCapture *allocateTouchCapture(long long) noexcept;
  [[nodiscard]] PresentationTouchEvent
  translateTouchEventForActiveTarget(const PresentationTouchEvent &) const;

  std::unique_ptr<PlayfieldPresentation> builtIn_;
  std::unique_ptr<CoordinatedPlaySkinSession> skin_;
  IGameplayBgaSubmitter &bga_;
  PersistGameplayViewport persistViewport_;
  std::function<void(const PresentationFailure &)> recordFailure_;
  PlayfieldPresentationConfig configuration_;
  std::vector<ReplayGhostEvent> replayGhostEvents_;
  std::optional<PendingFrame> pending_;
  std::optional<PresentationFailure> lastFailure_;
  long long lastEventMicros_ = 0;
  std::uint64_t lastFrameSerial_ = 0;
  mutable PresentationMode observedTouchMode_ = PresentationMode::BuiltIn;
  mutable std::uint64_t observedTargetLayoutRevision_ = 0;
  mutable std::uint64_t observedTargetHitRevision_ = 0;
  mutable std::uint64_t publishedLayoutRevision_ = 1;
  mutable std::uint64_t publishedHitRevision_ = 1;
  std::uint64_t touchTargetGeneration_ = 1;
  std::array<TouchCapture, gameplay::kRealtimeTouchFingerCapacity>
      touchCaptures_{};
};
