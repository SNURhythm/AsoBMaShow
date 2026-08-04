#pragma once

#include "PlayfieldPresentation.h"
#include "../../skin/SkinProfileSettings.h"
#include "../../skin/beatoraja/PlaySkinSession.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#if defined(ASOBMASHOW_PLAYFIELD_PRESENTATION_COORDINATOR_TESTING)
// A test-only boundary for the chart-lifetime session.  Production keeps the
// concrete, owning PlaySkinSession API below; focused coordinator tests do not
// need to reconstruct the Lua/resource graph merely to observe fan-out and
// atomic presentation selection.
class PlaySkinSessionForCoordinatorTesting {
public:
  virtual ~PlaySkinSessionForCoordinatorTesting() = default;

  [[nodiscard]] virtual const skin::PlaySkinSessionIdentity &
  identity() const noexcept = 0;
  [[nodiscard]] virtual PresentationFrameOutcome prepareFrame(
      const PlayfieldVisualState &, const PlayfieldProjectionResult &) = 0;
  [[nodiscard]] virtual PresentationFrameResult
  render(RenderContext &, const PreparedGameplayBgaFrame &,
         IGameplayBgaSubmitter &) = 0;
  virtual void setViewport(skin::ViewportSettings) = 0;
  virtual void updateViewportGeometry(skin::UiLogicalRect) = 0;
  [[nodiscard]] virtual gameplay::RealtimeTouchLayout touchLayout() const = 0;
  [[nodiscard]] virtual std::uint64_t
  touchLayoutRevision() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t
  touchHitRegionsRevision() const noexcept = 0;
  [[nodiscard]] virtual std::vector<PresentationUiHitRegion>
  touchHitRegions() const = 0;
  [[nodiscard]] virtual PresentationUiHit
  hitTestUiControl(UiLogicalPoint) const = 0;
  virtual PresentationTouchResult
  beginPresentationTouch(const PresentationTouchEvent &) = 0;
  virtual PresentationTouchResult
  updatePresentationTouch(const PresentationTouchEvent &) = 0;
  virtual PresentationTouchResult
  endPresentationTouch(const PresentationTouchEvent &, bool cancelled) = 0;
  virtual void cancelPresentationTouches(long long eventMicros) = 0;
  virtual void onLanePressed(int, JudgeResult, long long) = 0;
  virtual void onLaneReleased(int, long long) = 0;
  virtual void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock,
                       bool) = 0;
};

using CoordinatedPlaySkinSession = PlaySkinSessionForCoordinatorTesting;
#else
using CoordinatedPlaySkinSession = skin::PlaySkinSession;
#endif

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
};

// Owns both presentation candidates for one chart.  The built-in adapter is
// prepared first on every frame so a skin failure can select a complete,
// already-warmed fallback without evaluating gameplay state a second time.
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
  // pending input/frame geometry for both warmed presentation candidates.
  void updateSkinViewportGeometry(skin::UiLogicalRect safeUiBounds);

  void configure(const PlayfieldPresentationConfig &) override;
  [[nodiscard]] PresentationFrameOutcome prepareFrame(
      const PlayfieldVisualState &,
      const PlayfieldProjectionResult &) override;
  [[nodiscard]] PresentationFrameResult render(RenderContext &) override;
  [[nodiscard]] gameplay::RealtimeTouchLayout touchLayout() const override;
  [[nodiscard]] std::uint64_t
  touchLayoutRevision() const noexcept override;
  [[nodiscard]] std::uint64_t
  touchHitRegionsRevision() const noexcept override;
  [[nodiscard]] std::vector<PresentationUiHitRegion>
  touchHitRegions() const override;
  [[nodiscard]] PresentationUiHit
  hitTestUiControl(UiLogicalPoint) const override;
  PresentationTouchResult
  beginPresentationTouch(const PresentationTouchEvent &) override;
  PresentationTouchResult
  updatePresentationTouch(const PresentationTouchEvent &) override;
  PresentationTouchResult
  endPresentationTouch(const PresentationTouchEvent &, bool cancelled) override;
  void cancelPresentationTouches(long long eventMicros) override;
  void onLanePressed(int, JudgeResult, long long) override;
  void onLaneReleased(int, long long) override;
  void onJudge(JudgeResult, int, int, PlayfieldJudgeEventClock,
               bool) override;
  void reset() override;
  void refreshGeometry() override;
  [[nodiscard]] PresentationMode activeMode() const noexcept override;
  [[nodiscard]] std::optional<PresentationFailure>
  lastFailure() const override;

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
    std::optional<PreparedGameplayBgaFrame> bga;
    PreparationFailure preparationFailure = PreparationFailure::None;
    // Constructed before any fallible skin work. Once evaluation begins,
    // every no-submission decision can move one exact-identity payload into
    // fallback without allocating.
    PresentationFailure skinPrepareExceptionFailure;
    PresentationFailure bgaPrepareExceptionFailure;
    PresentationFailure skinRenderExceptionFailure;
    PresentationFailure skinNoSubmissionFailure;
  };
  struct TouchCapture {
    long long pointerId = 0;
    std::uint64_t targetGeneration = 0;
    bool active = false;
    PresentationUiHit publicHit;
  };

  [[nodiscard]] PresentationFrameResult
  renderBuiltIn(RenderContext &, PendingFrame,
                std::optional<PresentationFailure> selectedFailure,
                PresentationFrameOutcome selectedOutcome);
  [[nodiscard]] PresentationFailure
  makeSkinFailure(std::uint64_t frameSerial, skin::SkinDiagnostic) const;
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
