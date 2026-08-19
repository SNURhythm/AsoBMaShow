#pragma once

#include "PlayfieldPresentation.h"
#include "PlayfieldChartVisualModel.h"
#include "PlayfieldProjection.h"
#include "PlayfieldVisualState.h"
#include "../../skin/GameplaySkinActivationRequest.h"
#include "../../skin/SkinStoragePaths.h"
#include "../../skin/beatoraja/BeatorajaSkinConfiguration.h"
#include "../../skin/beatoraja/SkinDiagnosticHistory.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS ||                              \
    defined(ASOBMASHOW_GAMEPLAY_SKIN_SESSION_FACTORY_TESTING)
#include "../../skin/beatoraja/PlaySkinSession.h"
#else
namespace skin {
class PlaySkinSession;
class SkinConfigurationWriteQueue;
class SkinLiveResourceCounters;
class SkinResourcePreparationService;
} // namespace skin
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

struct GameplaySkinSessionServices {
  skin::AcquireGameplaySkinForNextChart acquire;
  const skin::SkinStorageRoots *storageRoots = nullptr;
  skin::SkinResourcePreparationService *resourcePreparation = nullptr;
  std::shared_ptr<skin::SkinLiveResourceCounters> liveResourceCounters;
  skin::SkinConfigurationWriteQueue *configurationWrites = nullptr;
  skin::SkinDiagnosticHistory *diagnosticHistory = nullptr;
  std::function<void(skin::SkinAudioVolumeWriterTarget, float)>
      applyAudioVolume;
  std::function<void(float)> applyPracticeItemScroll;

#if defined(ASOBMASHOW_GAMEPLAY_SKIN_SESSION_FACTORY_TESTING)
  std::function<skin::PlaySkinSessionCreateResult(skin::ValidatedSkinActivation,
                                                  skin::PlaySkinSessionContext)>
      createSessionForTesting;
#endif
};

struct GameplaySkinSessionInput {
  int keyMode = 0;
  const PlayfieldChartVisualModel *chartModel = nullptr;
  const PlayfieldVisualState *initialState = nullptr;
  const PlayfieldProjectionResult *initialProjection = nullptr;
  skin::UiLogicalRect safeUiBounds;
  std::optional<skin::RuntimeSkinConfigurationSelection>
      pinnedRuntimeSelection;
};

enum class GameplaySkinSessionDisposition : std::uint8_t {
  BuiltIn,
  Ready,
  Failed,
};

struct GameplaySkinSessionResult {
  GameplaySkinSessionDisposition disposition =
      GameplaySkinSessionDisposition::BuiltIn;
  std::unique_ptr<skin::PlaySkinSession> session;
  std::optional<PresentationFailure> failure;
  std::optional<skin::RuntimeSkinConfigurationSelection> runtimeSelection;
};

[[nodiscard]] GameplaySkinSessionResult
createGameplaySkinSession(GameplaySkinSessionServices services,
                          GameplaySkinSessionInput input);
