#pragma once

#include "PlaySkinStateBridge.h"
#include "Skin2DRenderer.h"
#include "SkinResourceCatalog.h"
#include "../SkinStoragePaths.h"
#include "../package/SkinActivationCommitStore.h"

#include <cstdint>
#include <memory>
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
  ViewportSettings viewport;
  UiLogicalRect safeUiBounds;
  SkinStorageRoots storageRoots;
  SkinResourcePreparationService &resourcePreparation;
  std::shared_ptr<SkinTextureDevice> textureDevice;
  std::stop_token stop;
};

struct PlaySkinSessionIdentity {
  std::uint64_t sessionSerial = 0;
  SkinProfileId profileId;
  SkinEntryId entry;
  std::string revisionDigest;
  std::string configurationDigest;
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
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  const SkinPreparedResourceView &resources;
  const PlaySkinViewport &viewport;
  LuaSkinRuntime &runtime;
  PlaySkinStateBridge &bridge;
  Skin2DRenderer &renderer;
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
class PlaySkinSession final {
public:
#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
  // Transaction-core seam retained only by the focused unit target. Runtime
  // application code can construct a session only through an owning
  // ValidatedSkinActivation.
  explicit PlaySkinSession(PlaySkinSessionFrameContext) noexcept;
#endif

  static PlaySkinSessionCreateResult create(ValidatedSkinActivation,
                                            PlaySkinSessionContext);
  ~PlaySkinSession();

  PlaySkinSession(const PlaySkinSession &) = delete;
  PlaySkinSession &operator=(const PlaySkinSession &) = delete;
  PlaySkinSession(PlaySkinSession &&) = delete;
  PlaySkinSession &operator=(PlaySkinSession &&) = delete;

  [[nodiscard]] PlaySkinFrameTransactionResult prepareFrame(
      const PlayfieldVisualState &, const PlayfieldProjectionResult &,
      std::span<const SkinWriterInvocation> queuedWriters);
  [[nodiscard]] const PlaySkinSessionIdentity &identity() const noexcept;

private:
  struct OwnedActivation;
  explicit PlaySkinSession(std::unique_ptr<OwnedActivation>) noexcept;

  // Declared before the borrowed frame context so every non-owning reference
  // is discarded before the activation graph. OwnedActivation itself declares
  // the master revision pin first, making it the final released member.
  std::unique_ptr<OwnedActivation> owned_;
  PlaySkinSessionFrameContext context_;
};

} // namespace skin
