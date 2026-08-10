#pragma once

#include "PlaySkinStateBridge.h"
#include "PlaySkinSessionIdentity.h"
#include "Skin2DRenderer.h"
#include "SkinResourceCatalog.h"
#include "../../scene/play/PlayfieldPresentation.h"
#include "../../scene/play/CoordinatedPlaySkinSession.h"
#include "../SkinConfigurationWriteQueue.h"
#include "../SkinStoragePaths.h"
#include "../package/SkinActivationCommitStore.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace skin {

struct PlaySkinSessionContext {
  std::uint64_t sessionSerial = 0;
  SkinProfileId profileId;
  // Immutable chart-lifetime snapshot. The caller owns it and must keep it
  // alive and unchanged until the resulting PlaySkinSession is destroyed.
  const PlayfieldChartVisualModel &chartModel;
  // The configured Lua pass runs with the same authoritative snapshot that
  // starts the attempt. These are optional only so create() can diagnose an
  // incomplete caller rather than fabricate state.
  const PlayfieldVisualState *initialState = nullptr;
  const PlayfieldProjectionResult *initialProjection = nullptr;
  ViewportSettings viewport;
  UiLogicalRect safeUiBounds;
  SkinStorageRoots storageRoots;
  SkinResourcePreparationService &resourcePreparation;
  std::shared_ptr<SkinTextureDevice> textureDevice;
  std::shared_ptr<SkinLiveResourceCounters> liveResourceCounters;
  SkinConfigurationWriteQueue &configurationWrites;
  std::stop_token stop;
};

class PlaySkinSession;

struct PlaySkinSessionCreateResult {
  std::unique_ptr<PlaySkinSession> session;
  EntryProfileSettings reconciledSettings;
  std::string configurationDigest;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

struct PlaySkinSessionFrameContext {
  std::uint64_t sessionSerial = 0;
  PlaySkinSessionIdentity identity;
  const PlayfieldChartVisualModel &chartModel;
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  const SkinPreparedResourceView &resources;
  ViewportSettings viewportSettings;
  PlaySkinViewport viewport;
  LuaSkinRuntime &runtime;
  PlaySkinStateBridge &bridge;
  Skin2DRenderer &renderer;
  rendering::SkinQuadBatchRenderer &quadRenderer;
  SkinConfigurationWriteQueue &configurationWrites;
  ISkinGaugeRandomSource *gaugeRandomSource = nullptr;
};

enum class PlaySkinFrameTransactionOutcome : std::uint8_t {
  Rejected,
  Ready,
};

// Entirely value-owned publication. A later coordinator may submit the
// evaluation and enqueue the committed mutations without retaining any frame
// state, projection, callback, or input span borrowed by prepareFrame.
struct PlaySkinFrameTransactionResult {
  std::uint64_t frameSerial = 0;
  PlaySkinFrameTransactionOutcome outcome =
      PlaySkinFrameTransactionOutcome::Rejected;
  SkinFrameEvaluationResult evaluation;
  PlaySkinFrameCommit committed;
  std::vector<SkinDiagnostic> diagnostics;

  [[nodiscard]] bool ready() const noexcept {
    return outcome == PlaySkinFrameTransactionOutcome::Ready &&
           evaluation.submitReady.has_value() &&
           committed.frameSerial == frameSerial && frameSerial != 0;
  }
};

// Frame-transaction core with activation/resource ownership. Coordinator
// submission is deliberately added by later Task 21 slices.
class PlaySkinSession final : public CoordinatedPlaySkinSession {
public:
#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
  // Transaction-core seam retained only by the focused unit target. Runtime
  // application code can construct a session only through an owning
  // ValidatedSkinActivation.
  explicit PlaySkinSession(PlaySkinSessionFrameContext) noexcept;
#endif

  static PlaySkinSessionCreateResult create(ValidatedSkinActivation,
                                            PlaySkinSessionContext);
  ~PlaySkinSession() override;

  PlaySkinSession(const PlaySkinSession &) = delete;
  PlaySkinSession &operator=(const PlaySkinSession &) = delete;
  PlaySkinSession(PlaySkinSession &&) = delete;
  PlaySkinSession &operator=(PlaySkinSession &&) = delete;

  [[nodiscard]] PresentationFrameOutcome prepareFrame(
      const PlayfieldVisualState &,
      const PlayfieldProjectionResult &) override;
  [[nodiscard]] PresentationFrameResult
  render(RenderContext &, const PreparedGameplayBgaFrame &,
         IGameplayBgaSubmitter &) override;
  void submitSyntheticReplayGhosts(
      RenderContext &, const SyntheticReplayGhostFrameInput &) override;
  void setViewport(ViewportSettings) override;
  // Geometry-only chart-lifetime refresh for safe-area/rotation changes. This
  // preserves the selected viewport settings and immutable activation identity
  // while invalidating every input capture and pending old-geometry frame.
  void updateViewportGeometry(UiLogicalRect safeUiBounds) override;
  [[nodiscard]] const PlaySkinSessionIdentity &
  identity() const noexcept override;
  [[nodiscard]] gameplay::RealtimeTouchLayout touchLayout() const override;
  [[nodiscard]] std::uint64_t
  touchLayoutRevision() const noexcept override;
  [[nodiscard]] std::uint64_t
  touchHitRegionsRevision() const noexcept override;
  [[nodiscard]] std::vector<PresentationUiHitRegion>
  touchHitRegions() const override;
  [[nodiscard]] PresentationUiHit
  hitTestUiControl(UiLogicalPoint point) const override;
  PresentationTouchResult
  beginPresentationTouch(const PresentationTouchEvent &event) override;
  PresentationTouchResult
  updatePresentationTouch(const PresentationTouchEvent &event) override;
  PresentationTouchResult
  endPresentationTouch(const PresentationTouchEvent &event,
                       bool cancelled) override;
  void cancelPresentationTouches(long long eventMicros) override;
  // Task 20's bounded Down queue and next-frame transactional drain supersede
  // the earlier immediate invokeWriter/SkinWriterResult plan surface. Touch
  // callbacks never mutate gameplay authority in the input callback itself.
  // Gameplay authority is captured once in PlayfieldVisualState; these event
  // hooks intentionally retain no parallel mutable state, but keep the
  // coordinator fan-out surface concrete and exact-once callable.
  void onLanePressed(int lane, JudgeResult judge,
                     long long eventMicros) override;
  void onLaneReleased(int lane, long long eventMicros) override;
  void onJudge(JudgeResult judge, int combo, int score,
               PlayfieldJudgeEventClock clock,
               bool recordTimingSample) override;

#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
  // Focused transaction-core seam. Application code cannot supply an
  // external writer span or observe an unpublished bridge commit.
  [[nodiscard]] PlaySkinFrameTransactionResult prepareFrame(
      const PlayfieldVisualState &, const PlayfieldProjectionResult &,
      std::span<const SkinWriterInvocation> queuedWriters);
  // Pinned v1 has no persisted mutation selector. This narrow seam proves the
  // forward-compatible splitter only after the normal pending-frame
  // evaluation and renderer submission have succeeded.
  [[nodiscard]] PresentationFrameOutcome prepareFrameForTesting(
      const PlayfieldVisualState &, const PlayfieldProjectionResult &,
      std::span<const SkinWriterInvocation>,
      std::span<const SkinFrameMutation> extraMutations);
#endif

private:
  struct OwnedActivation;
  struct PendingFrame {
    PlaySkinFrameTransactionResult transaction;
  };
  struct TouchCapture {
    long long pointerId = 0;
    bool active = false;
    PresentationUiHit hit;
  };

  explicit PlaySkinSession(std::unique_ptr<OwnedActivation>);
  [[nodiscard]] PlaySkinFrameTransactionResult runFrameTransaction(
      const PlayfieldVisualState &, const PlayfieldProjectionResult &,
      std::span<const SkinWriterInvocation>,
      std::span<const SkinEventInvocation> = {});
  [[nodiscard]] PresentationFrameOutcome preparePendingFrame(
      const PlayfieldVisualState &, const PlayfieldProjectionResult &,
      std::span<const SkinWriterInvocation>,
      std::span<const SkinEventInvocation> = {},
      std::span<const SkinFrameMutation> extraMutations = {});
  void clearPublishedGeometry(bool advanceTopology) noexcept;

  // Declared before the borrowed frame context so every non-owning reference
  // is discarded before the activation graph. OwnedActivation itself declares
  // the master revision pin first, making it the final released member.
  std::unique_ptr<OwnedActivation> owned_;
  PlaySkinSessionFrameContext context_;
  // These value-owned consumers are declared after every borrowed frame
  // reference so reverse destruction discards them before context_/owned_.
  std::optional<PendingFrame> pendingFrame_;
  std::array<TouchCapture, gameplay::kRealtimeTouchFingerCapacity> captures_;
  std::optional<SkinInteractionLayout> publishedLayout_;
  std::optional<SyntheticReplayGhostGeometry> publishedReplayGhostGeometry_;
  static constexpr std::size_t maximumQueuedInteractions = 256;
  std::array<SkinWriterInvocation, maximumQueuedInteractions> queuedWriters_{};
  std::size_t queuedWriterCount_ = 0;
  std::array<SkinEventInvocation, maximumQueuedInteractions> queuedEvents_{};
  std::size_t queuedEventCount_ = 0;
  std::uint64_t touchLayoutRevision_ = 1;
  std::uint64_t touchHitRegionsRevision_ = 1;
};

} // namespace skin
