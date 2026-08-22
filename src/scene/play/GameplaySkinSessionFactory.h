#pragma once

#include "PlayfieldPresentation.h"
#include "PlayfieldChartVisualModel.h"
#include "PlayfieldProjection.h"
#include "PlayfieldVisualState.h"
#include "../../skin/GameplaySkinActivationRequest.h"
#include "../../skin/SkinStoragePaths.h"
#include "../../skin/beatoraja/BeatorajaSkinConfiguration.h"
#include "../../skin/beatoraja/SkinDiagnosticHistory.h"

#include <cstdint>

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS ||                              \
    defined(ASOBMASHOW_GAMEPLAY_SKIN_SESSION_FACTORY_TESTING)
#include "../../skin/beatoraja/PlaySkinSession.h"
#else
namespace skin {
enum class SkinAudioVolumeWriterTarget : std::uint8_t;
class PlaySkinSession;
class SkinConfigurationWriteQueue;
class SkinLiveResourceCounters;
class LuaSkinAudioBackend;
class LuaSkinHttpTransport;
class SkinResourcePreparationService;
} // namespace skin
#endif

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

class GameplaySkinSessionStopOwner final {
public:
  GameplaySkinSessionStopOwner() = default;
  GameplaySkinSessionStopOwner(const GameplaySkinSessionStopOwner &) = delete;
  GameplaySkinSessionStopOwner &
  operator=(const GameplaySkinSessionStopOwner &) = delete;
  ~GameplaySkinSessionStopOwner() { requestStop(); }

  [[nodiscard]] std::stop_token token() const noexcept {
    return source_.get_token();
  }
  void requestStop() noexcept { source_.request_stop(); }
  void resetForNextSession() noexcept {
    source_.request_stop();
    source_ = std::stop_source{};
  }

private:
  std::stop_source source_;
};

struct GameplaySkinSessionServices {
  skin::AcquireGameplaySkinForNextChart acquire;
  const skin::SkinStorageRoots *storageRoots = nullptr;
  skin::SkinResourcePreparationService *resourcePreparation = nullptr;
  skin::SkinBuiltinImageReader builtinImageReader;
  std::shared_ptr<skin::SkinLiveResourceCounters> liveResourceCounters;
  std::function<std::unique_ptr<skin::LuaSkinHttpTransport>(std::stop_token)>
      createHttpTransport;
  std::shared_ptr<skin::LuaSkinAudioBackend> audioBackend;
  std::function<skin::LuaSkinLegacyInputGeneration()>
      captureLegacyInputGeneration;
  skin::SkinConfigurationWriteQueue *configurationWrites = nullptr;
  skin::SkinDiagnosticHistory *diagnosticHistory = nullptr;
  std::function<void(skin::SkinAudioVolumeWriterTarget, float)>
      applyAudioVolume;
  std::function<void(float)> applyPracticeItemScroll;
  std::function<void(std::size_t, bool)> applyPracticeMenuItem;
  std::function<void(int)> applyPracticeVisibleItems;
  std::stop_token stop;

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
