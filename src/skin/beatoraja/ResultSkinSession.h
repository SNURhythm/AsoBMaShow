#pragma once

#include "GameplaySkinDocumentLoader.h"
#include "LuaSkinAudioHost.h"
#include "ResultSkinStateBridge.h"
#include "Skin2DRenderer.h"
#include "SkinMovieCatalog.h"
#include "SkinResourceCatalog.h"

#include "../SkinStoragePaths.h"
#include "../package/SkinActivationCommitStore.h"

#include <chrono>
#include <memory>
#include <stop_token>
#include <unordered_map>
#include <vector>

struct RenderContext;

namespace rendering { class SkinQuadBatchRenderer; }

namespace skin {

struct ResultSkinSessionContext {
  // Exact Beatoraja SkinType selected for this result surface (7 or 15).
  // Zero is retained only for older test callers that do not select a target.
  int expectedSkinType = 0;
  SkinProfileId profileId;
  SkinStorageRoots storageRoots;
  SkinResourcePreparationService &resourcePreparation;
  // Beatoraja runs configured Lua against the active result MainState. Keep
  // the immutable result data available for that one-time configured phase.
  ResultSkinData initialData;
  std::shared_ptr<SkinTextureDevice> textureDevice;
  std::shared_ptr<SkinMovieDevice> movieDevice;
  SkinBuiltinImageReader builtinImageReader;
  std::shared_ptr<LuaSkinAudioBackend> audioBackend;
  std::shared_ptr<SkinLiveResourceCounters> liveResourceCounters;
  SkinSafetyPolicy safetyPolicy{};
  std::stop_token stop;
};

struct ResultSkinSessionCreateResult {
  std::unique_ptr<class ResultSkinSession> session;
  std::vector<SkinDiagnostic> diagnostics;
};

class ResultSkinSession final {
public:
  static ResultSkinSessionCreateResult create(ValidatedSkinActivation,
                                              ResultSkinSessionContext);
  ~ResultSkinSession();

  ResultSkinSession(const ResultSkinSession &) = delete;
  ResultSkinSession &operator=(const ResultSkinSession &) = delete;

  [[nodiscard]] bool render(RenderContext &, const ResultSkinData &,
                            std::uint64_t frameSerial,
                            std::int64_t elapsedMillis);
  [[nodiscard]] bool
  requiresRuntimeStringRefresh(const ResultSkinData &) const;
  [[nodiscard]] std::vector<SkinDiagnostic> takeLastDiagnostics();
  [[nodiscard]] std::vector<int> takeQueuedBuiltinEventIds();
  [[nodiscard]] bool queuePointerDown(UiLogicalPoint, long long eventMicros);
  [[nodiscard]] const SkinEntryId &entry() const noexcept;

private:
  ResultSkinSession(SkinRevisionLease, SkinEntryId,
                    ValidatedBeatorajaSkinModel, BeatorajaSkinConfiguration,
                    std::unique_ptr<LuaSkinRuntime>,
                    std::unique_ptr<SkinResourceCatalog>,
                    std::unique_ptr<SkinMovieCatalog>,
                    SkinSafetyPolicy, ViewportSettings,
                    std::vector<std::string> preparedRuntimeStrings);

  SkinRevisionLease revision_;
  SkinEntryId entry_;
  ValidatedBeatorajaSkinModel model_;
  BeatorajaSkinConfiguration configuration_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::unique_ptr<SkinResourceCatalog> resources_;
  std::unique_ptr<SkinMovieCatalog> movies_;
  std::unique_ptr<ISkinGaugeRandomSource> gaugeRandom_;
  SkinSafetyPolicy safetyPolicy_{};
  ViewportSettings viewportSettings_{};
  Skin2DRenderer renderer_;
  std::unique_ptr<rendering::SkinQuadBatchRenderer> quadRenderer_;
  std::vector<SkinDiagnostic> lastDiagnostics_;
  std::optional<SkinInteractionLayout> publishedInteractionLayout_;
  std::vector<SkinEventInvocation> queuedEventInvocations_;
  std::vector<int> queuedBuiltinEventIds_;
  std::unordered_map<int, std::int64_t> customEventLastExecutionMicros_;
  std::vector<std::string> preparedRuntimeStrings_;
};

} // namespace skin
