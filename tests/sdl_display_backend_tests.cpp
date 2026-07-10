#include "video/SDLDisplayBackend.h"
#include "video/FramePacer.h"

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using player_settings::DisplayMode;
using player_settings::VideoSettings;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

class FakeSDLAdapter final : public display::ISDLDisplayAdapter {
public:
  display::SDLWindowState state{
      .mode = DisplayMode::Windowed,
      .displayIndex = 0,
      .width = 1280,
      .height = 720,
      .x = 100,
      .y = 200,
      .windowFlags = 0x20,
  };
  std::vector<display::SDLDisplayBounds> bounds{{0, 0, 1920, 1080},
                                                {1920, 0, 2560, 1440}};
  std::vector<std::vector<display::SDLNativeDisplayMode>> modes{
      {{1280, 720, 60, 1}, {1920, 1080, 60, 1}, {1920, 1080, 144, 1}},
      {{2560, 1440, 60, 1}},
  };
  std::optional<display::SDLNativeDisplayMode> forcedEffectiveMode;
  bool ignorePosition = false;
  int fullscreenCalls = 0;
  int clearModeCalls = 0;
  int sizeCalls = 0;
  int positionCalls = 0;
  int displayModeCalls = 0;
  std::optional<display::SDLNativeDisplayMode> requestedMode;
  std::optional<display::SDLNativeDisplayMode> lastRequestedMode;

  int displayCount() const override { return static_cast<int>(bounds.size()); }
  std::string displayName(int index) const override {
    return "Fake " + std::to_string(index);
  }
  std::vector<display::SDLNativeDisplayMode>
  displayModes(int index) const override {
    return modes.at(static_cast<std::size_t>(index));
  }
  std::optional<display::SDLNativeDisplayMode>
  desktopDisplayMode(int index) const override {
    const auto displayModes = modes.at(static_cast<std::size_t>(index));
    if (displayModes.empty()) {
      return std::nullopt;
    }
    return displayModes.back();
  }
  std::optional<display::SDLDisplayBounds>
  displayBounds(int index, std::string &errorMessage) const override {
    if (index < 0 || index >= displayCount()) {
      errorMessage = "missing bounds";
      return std::nullopt;
    }
    return bounds[static_cast<std::size_t>(index)];
  }
  display::SDLWindowState windowState() const override { return state; }
  bool setFullscreenMode(DisplayMode mode,
                         std::string & /*errorMessage*/) override {
    ++fullscreenCalls;
    state.mode = mode;
    if (mode == DisplayMode::BorderlessFullscreen) {
      const auto &displayBounds = bounds.at(state.displayIndex);
      state.width = displayBounds.width;
      state.height = displayBounds.height;
      state.effectiveDisplayMode.reset();
    } else if (mode == DisplayMode::ExclusiveFullscreen) {
      state.effectiveDisplayMode =
          forcedEffectiveMode.has_value() ? forcedEffectiveMode : requestedMode;
    } else {
      state.effectiveDisplayMode.reset();
    }
    return true;
  }
  bool clearWindowDisplayMode(std::string & /*errorMessage*/) override {
    ++clearModeCalls;
    state.effectiveDisplayMode.reset();
    requestedMode.reset();
    return true;
  }
  void setWindowSize(int width, int height) override {
    ++sizeCalls;
    state.width = width;
    state.height = height;
  }
  void setWindowPosition(int x, int y) override {
    ++positionCalls;
    if (!ignorePosition) {
      state.x = x;
      state.y = y;
    }
    for (std::size_t index = 0; index < bounds.size(); ++index) {
      if (x >= bounds[index].x && x < bounds[index].x + bounds[index].width) {
        state.displayIndex = static_cast<int>(index);
        break;
      }
    }
  }
  bool setWindowDisplayMode(const display::SDLNativeDisplayMode &mode,
                            std::string & /*errorMessage*/) override {
    ++displayModeCalls;
    requestedMode = mode;
    lastRequestedMode = mode;
    return true;
  }
};

struct RendererSpy {
  bool preflightSucceeds = true;
  bool synchronizeSucceeds = true;
  bool failNextSynchronize = false;
  bool rejectPreflightAfterSynchronizeFailure = false;
  int preflightCalls = 0;
  int synchronizeCalls = 0;
  std::uint32_t lastFlags = 0;

  bool preflight(std::uint32_t flags, std::string &errorMessage) {
    ++preflightCalls;
    lastFlags = flags;
    if (!preflightSucceeds) {
      errorMessage = "export active";
      return false;
    }
    return true;
  }

  bool synchronize(std::uint32_t flags, std::string &errorMessage) {
    ++synchronizeCalls;
    lastFlags = flags;
    if (!synchronizeSucceeds || std::exchange(failNextSynchronize, false)) {
      if (rejectPreflightAfterSynchronizeFailure) {
        preflightSucceeds = false;
      }
      errorMessage = "invalid drawable";
      return false;
    }
    return true;
  }
};

display::SDLDisplayBackend
makeBackend(const std::shared_ptr<FakeSDLAdapter> &adapter,
            RendererSpy &renderer, std::uint32_t &activeFlags) {
  return display::SDLDisplayBackend(
      adapter, false, [&activeFlags]() { return activeFlags; },
      [&renderer](std::uint32_t flags, std::string &errorMessage) {
        return renderer.preflight(flags, errorMessage);
      },
      [&renderer, &activeFlags](std::uint32_t flags,
                                std::string &errorMessage) {
        if (!renderer.synchronize(flags, errorMessage)) {
          return false;
        }
        activeFlags = flags;
        return true;
      });
}

VideoSettings resizedWindow() {
  VideoSettings settings;
  settings.width = 1600;
  settings.height = 900;
  return settings;
}

void testDisplayMutationSynchronizesRendererWithUnchangedFlags() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  const auto previous = backend.capture();
  std::string errorMessage;

  require(backend.apply(resizedWindow(), errorMessage),
          "window resize succeeds through the concrete transaction");
  require(renderer.preflightCalls == 1 && renderer.synchronizeCalls == 1,
          "display mutation synchronizes even when VSync flags are unchanged");
  require(renderer.lastFlags == 0x40,
          "unchanged non-VSync renderer flags are preserved");
  require(backend.restore(previous, errorMessage),
          "display restore succeeds with unchanged renderer flags");
  require(renderer.preflightCalls == 2 && renderer.synchronizeCalls == 2,
          "actual restore synchronizes even when VSync flags are unchanged");
}

void testPreflightRejectsBeforeAnySDLMutation() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  renderer.preflightSucceeds = false;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  std::string errorMessage;

  require(!backend.apply(resizedWindow(), errorMessage),
          "export preflight rejects the display transaction");
  require(adapter->fullscreenCalls == 0 && adapter->clearModeCalls == 0 &&
              adapter->sizeCalls == 0 && adapter->positionCalls == 0,
          "preflight failure leaves SDL untouched");
  require(renderer.synchronizeCalls == 0,
          "preflight failure never enters renderer synchronization");
}

void testRendererFailureCanRestoreSDLAndRendererCoherently() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  renderer.synchronizeSucceeds = false;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  const auto previous = backend.capture();
  std::string errorMessage;

  require(!backend.apply(resizedWindow(), errorMessage),
          "injected invalid drawable fails after the SDL candidate mutation");
  require(adapter->state.width == 1280 && adapter->state.height == 720 &&
              adapter->state.x == 100 && adapter->state.y == 200,
          "renderer failure atomically restores the SDL window");
  require(renderer.synchronizeCalls == 1,
          "failed renderer synchronization is not replayed during SDL repair");
  require(backend.capture().settings == previous.settings,
          "failed apply leaves the captured effective display settings");
}

void testManagerRollsConcreteSDLTransactionBackAfterRendererFailure() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  renderer.failNextSynchronize = true;
  renderer.rejectPreflightAfterSynchronizeFailure = true;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  FramePacer pacer;
  display::DisplaySettingsManager manager(backend, pacer, VideoSettings{});

  auto candidate = resizedWindow();
  candidate.width = 1920;
  candidate.height = 1080;
  const auto result =
      manager.beginPreview(candidate, std::chrono::steady_clock::now());
  require(result.status == display::ApplyStatus::FailedRolledBack,
          "manager reports a rolled-back concrete renderer failure");
  require(adapter->state.width == 1280 && adapter->state.height == 720 &&
              adapter->state.x == 100 && adapter->state.y == 200,
          "manager rollback restores concrete SDL state");
  require(renderer.preflightCalls == 1 && renderer.synchronizeCalls == 1,
          "post-preflight export race cannot block the atomic SDL repair");
}

void testExclusiveVerificationRejectsAcceptedLowerRefresh() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  adapter->forcedEffectiveMode =
      display::SDLNativeDisplayMode{1920, 1080, 60, 1};
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  auto candidate = resizedWindow();
  candidate.mode = DisplayMode::ExclusiveFullscreen;
  candidate.width = 1920;
  candidate.height = 1080;
  std::string errorMessage;

  require(!backend.apply(candidate, errorMessage),
          "accepted lower refresh fails exclusive postcondition checking");
  require(adapter->lastRequestedMode.has_value() &&
              adapter->lastRequestedMode->refreshRateHz == 144,
          "backend requests the highest matching refresh rate");
  require(renderer.synchronizeCalls == 0,
          "failed SDL verification cannot commit renderer state");
}

void testVsyncOnlyExclusiveChangeVerifiesTheEffectiveMode() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  adapter->state.mode = DisplayMode::ExclusiveFullscreen;
  adapter->state.width = 1920;
  adapter->state.height = 1080;
  adapter->state.effectiveDisplayMode =
      display::SDLNativeDisplayMode{1920, 1080, 144, 1};
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  VideoSettings candidate;
  candidate.mode = DisplayMode::ExclusiveFullscreen;
  candidate.width = 1920;
  candidate.height = 1080;
  candidate.vsync = true;
  std::string errorMessage;

  require(backend.apply(candidate, errorMessage),
          "VSync-only exclusive apply verifies the already active mode");
  require(adapter->fullscreenCalls == 0 && renderer.synchronizeCalls == 1,
          "VSync-only exclusive apply does not rebuild the SDL mode");
  require((activeFlags & 0x40) != 0 && activeFlags != 0x40,
          "VSync toggle preserves unrelated renderer reset flags");
}

void testWindowedRestoreRejectsIgnoredPosition() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  const auto previous = backend.capture();
  adapter->state.width = 1600;
  adapter->state.height = 900;
  adapter->state.x = 300;
  adapter->state.y = 400;
  adapter->ignorePosition = true;
  std::string errorMessage;

  require(!backend.restore(previous, errorMessage),
          "windowed restore reports a window manager that ignored position");
  require(renderer.synchronizeCalls == 0,
          "failed position verification cannot commit renderer state");
}

void testCapabilitiesUseTheSDLAdapterAndDeduplicateModes() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  adapter->modes[0].push_back({1920, 1080, 144, 1});
  adapter->modes[0].push_back({0, 1080, 60, 1});
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);

  const auto capabilities = backend.capabilities();
  require(capabilities.displays.size() == 2,
          "adapter-backed capabilities enumerate both displays");
  require(capabilities.displays[0].resolutions.size() == 3,
          "capabilities discard invalid and duplicate SDL modes");
}

void testFixedMobileDisplayOnlyAdvertisesFrameCap() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  adapter->state.mode = DisplayMode::ExclusiveFullscreen;
  adapter->state.displayIndex = 1;
  adapter->state.width = 2560;
  adapter->state.height = 1440;
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  display::SDLDisplayBackend backend(
      adapter, true, [&activeFlags]() { return activeFlags; },
      [&renderer](std::uint32_t flags, std::string &errorMessage) {
        return renderer.preflight(flags, errorMessage);
      },
      [&renderer](std::uint32_t flags, std::string &errorMessage) {
        return renderer.synchronize(flags, errorMessage);
      });

  const auto capabilities = backend.capabilities();
  require(!capabilities.canChangeMode && !capabilities.canSelectDisplay &&
              !capabilities.canSelectResolution &&
              !capabilities.canChangeVsync && capabilities.canSetFrameCap,
          "mobile keeps its fixed display and app-owned frame cap contract");
  require(capabilities.displays.size() == 1 &&
              capabilities.displays[0].index == 0,
          "mobile exposes only its current physical display as logical zero");
}
} // namespace

int main() {
  testDisplayMutationSynchronizesRendererWithUnchangedFlags();
  testPreflightRejectsBeforeAnySDLMutation();
  testRendererFailureCanRestoreSDLAndRendererCoherently();
  testManagerRollsConcreteSDLTransactionBackAfterRendererFailure();
  testExclusiveVerificationRejectsAcceptedLowerRefresh();
  testVsyncOnlyExclusiveChangeVerifiesTheEffectiveMode();
  testWindowedRestoreRejectsIgnoredPosition();
  testCapabilitiesUseTheSDLAdapterAndDeduplicateModes();
  testFixedMobileDisplayOnlyAdvertisesFrameCap();
  return 0;
}
