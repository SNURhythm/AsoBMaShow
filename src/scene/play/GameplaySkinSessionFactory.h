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
#include <mutex>
#include <optional>
#include <stop_token>

class GameplaySkinSessionStopHandle final {
public:
  GameplaySkinSessionStopHandle() = default;

  [[nodiscard]] std::stop_token token() const noexcept {
    return source_.get_token();
  }
  void requestStop() const noexcept { source_.request_stop(); }

private:
  friend class GameplaySkinSessionStopOwner;
  explicit GameplaySkinSessionStopHandle(std::stop_source source) noexcept
      : source_(std::move(source)) {}

  mutable std::stop_source source_{std::nostopstate};
};

class GameplaySkinSessionStopOwner final {
public:
  GameplaySkinSessionStopOwner() = default;
  explicit GameplaySkinSessionStopOwner(std::stop_token upstream)
      : upstream_(upstream) {
    connectUpstream();
  }
  GameplaySkinSessionStopOwner(const GameplaySkinSessionStopOwner &) = delete;
  GameplaySkinSessionStopOwner &
  operator=(const GameplaySkinSessionStopOwner &) = delete;
  ~GameplaySkinSessionStopOwner() { requestStop(); }

  [[nodiscard]] std::stop_token token() const noexcept {
    std::lock_guard lock(mutex_);
    return source_.get_token();
  }
  [[nodiscard]] GameplaySkinSessionStopHandle handle() const noexcept {
    std::lock_guard lock(mutex_);
    return GameplaySkinSessionStopHandle(source_);
  }
  void requestStop() noexcept {
    std::stop_source source;
    {
      std::lock_guard lock(mutex_);
      source = source_;
    }
    source.request_stop();
  }
  void resetForNextSession() noexcept {
    std::lock_guard lock(mutex_);
    source_.request_stop();
    source_ = std::stop_source{};
    connectUpstream();
  }

private:
  using UpstreamCallback = std::stop_callback<std::function<void()>>;

  void connectUpstream() {
    upstreamCallback_.reset();
    if (!upstream_.stop_possible()) {
      return;
    }
    upstreamCallback_ = std::make_unique<UpstreamCallback>(
        upstream_, [source = source_]() mutable { source.request_stop(); });
  }

  mutable std::mutex mutex_;
  std::stop_source source_;
  std::stop_token upstream_;
  std::unique_ptr<UpstreamCallback> upstreamCallback_;
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
