#pragma once

#include "GameplaySkinDocumentLoader.h"
#include "ResultSkinStateBridge.h"
#include "Skin2DRenderer.h"
#include "SkinResourceCatalog.h"

#include "../SkinStoragePaths.h"
#include "../package/SkinActivationCommitStore.h"

#include <chrono>
#include <memory>
#include <stop_token>

struct RenderContext;

namespace rendering { class SkinQuadBatchRenderer; }

namespace skin {

struct ResultSkinSessionContext {
  SkinProfileId profileId;
  SkinStorageRoots storageRoots;
  SkinResourcePreparationService &resourcePreparation;
  // Beatoraja runs configured Lua against the active result MainState. Keep
  // the immutable result data available for that one-time configured phase.
  ResultSkinData initialData;
  std::shared_ptr<SkinTextureDevice> textureDevice;
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
  [[nodiscard]] const SkinEntryId &entry() const noexcept;

private:
  ResultSkinSession(SkinRevisionLease, SkinEntryId,
                    ValidatedBeatorajaSkinModel, BeatorajaSkinConfiguration,
                    std::unique_ptr<LuaSkinRuntime>,
                    std::unique_ptr<SkinResourceCatalog>,
                    SkinSafetyPolicy);

  SkinRevisionLease revision_;
  SkinEntryId entry_;
  ValidatedBeatorajaSkinModel model_;
  BeatorajaSkinConfiguration configuration_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::unique_ptr<SkinResourceCatalog> resources_;
  SkinSafetyPolicy safetyPolicy_{};
  Skin2DRenderer renderer_;
  std::unique_ptr<rendering::SkinQuadBatchRenderer> quadRenderer_;
};

} // namespace skin
