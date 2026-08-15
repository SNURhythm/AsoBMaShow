#include "video/FramePacer.h"
#include "video/RendererAccessCoordinator.h"
#include "video/SDLDisplayBackend.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
      .maximized = false,
  };
  std::vector<display::SDLDisplayBounds> bounds{{0, 0, 1920, 1080},
                                                {1920, 0, 2560, 1440}};
  std::vector<std::vector<display::SDLNativeDisplayMode>> modes{
      {{1280, 720, 60, 1}, {1920, 1080, 60, 1}, {1920, 1080, 144, 1}},
      {{2560, 1440, 60, 1}},
  };
  std::vector<display::SDLNativeDisplayMode> currentPhysicalModes{
      {1920, 1080, 144, 1}, {2560, 1440, 60, 1}};
  bool ignorePosition = false;
  bool ignoreMaximize = false;
  std::function<void()> beforeFirstMutation;
  std::function<void()> mutationObserver;
  bool mutationHookCalled = false;
  int fullscreenCalls = 0;
  int clearModeCalls = 0;
  int sizeCalls = 0;
  int positionCalls = 0;
  int displayModeCalls = 0;
  int maximizeCalls = 0;
  std::optional<display::SDLNativeDisplayMode> requestedMode;
  std::optional<display::SDLNativeDisplayMode> lastRequestedMode;
  std::optional<display::SDLWindowState> restoredWindowState;

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
  std::optional<display::SDLNativeDisplayMode>
  currentDisplayMode(int index) const override {
    if (index < 0 || index >= static_cast<int>(currentPhysicalModes.size())) {
      return std::nullopt;
    }
    return currentPhysicalModes[static_cast<std::size_t>(index)];
  }
  void beforeMutation() {
    if (!mutationHookCalled && beforeFirstMutation) {
      mutationHookCalled = true;
      beforeFirstMutation();
    }
    if (mutationObserver) {
      mutationObserver();
    }
  }
  bool setFullscreenMode(DisplayMode mode,
                         std::string & /*errorMessage*/) override {
    beforeMutation();
    ++fullscreenCalls;
    state.mode = mode;
    if (mode == DisplayMode::BorderlessFullscreen) {
      const auto &displayBounds = bounds.at(state.displayIndex);
      state.width = displayBounds.width;
      state.height = displayBounds.height;
      state.requestedWindowMode.reset();
    } else if (mode == DisplayMode::ExclusiveFullscreen) {
      state.requestedWindowMode = requestedMode;
    } else {
      state.requestedWindowMode.reset();
    }
    return true;
  }
  bool clearWindowDisplayMode(std::string & /*errorMessage*/) override {
    beforeMutation();
    ++clearModeCalls;
    state.requestedWindowMode.reset();
    requestedMode.reset();
    return true;
  }
  void setWindowSize(int width, int height) override {
    beforeMutation();
    ++sizeCalls;
    state.width = width;
    state.height = height;
    if (!state.maximized && state.mode == DisplayMode::Windowed) {
      restoredWindowState = state;
    }
  }
  void setWindowPosition(int x, int y) override {
    beforeMutation();
    ++positionCalls;
    if (!ignorePosition) {
      state.x = x;
      state.y = y;
      if (!state.maximized && state.mode == DisplayMode::Windowed) {
        restoredWindowState = state;
      }
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
    beforeMutation();
    ++displayModeCalls;
    requestedMode = mode;
    lastRequestedMode = mode;
    return true;
  }
  void setWindowMaximized(bool maximized) override {
    beforeMutation();
    ++maximizeCalls;
    if (!ignoreMaximize) {
      if (state.maximized && !maximized && restoredWindowState.has_value()) {
        const bool wasMaximized = state.maximized;
        state = *restoredWindowState;
        state.maximized = wasMaximized;
      } else if (!state.maximized && maximized) {
        restoredWindowState = state;
        const auto &displayBounds = bounds.at(state.displayIndex);
        state.x = displayBounds.x;
        state.y = displayBounds.y;
        state.width = displayBounds.width;
        state.height = displayBounds.height;
      }
      state.maximized = maximized;
    }
  }
};

struct RendererSpy {
  bool reservationSucceeds = true;
  bool synchronizeSucceeds = true;
  bool failNextSynchronize = false;
  int reservationCalls = 0;
  int synchronizeCalls = 0;
  std::uint32_t lastFlags = 0;

  bool reserve(std::uint32_t flags, std::string &errorMessage) {
    ++reservationCalls;
    lastFlags = flags;
    if (!reservationSucceeds) {
      errorMessage = "export active";
      return false;
    }
    return true;
  }

  bool synchronize(std::uint32_t flags, std::string &errorMessage) {
    ++synchronizeCalls;
    lastFlags = flags;
    if (!synchronizeSucceeds || std::exchange(failNextSynchronize, false)) {
      errorMessage = "invalid drawable";
      return false;
    }
    return true;
  }
};

class FakeRendererTransaction final
    : public display::IRendererDisplayTransaction {
public:
  FakeRendererTransaction(RendererSpy &rendererValue,
                          std::shared_ptr<void> lifetimeValue = {})
      : renderer(rendererValue), lifetime(std::move(lifetimeValue)) {}

  bool synchronize(std::uint32_t flags, std::string &errorMessage) override {
    return renderer.synchronize(flags, errorMessage);
  }

private:
  RendererSpy &renderer;
  std::shared_ptr<void> lifetime;
};

class CoordinatedRendererTransaction final
    : public display::IRendererDisplayTransaction {
public:
  CoordinatedRendererTransaction(
      RendererSpy &rendererValue, std::uint32_t &activeFlagsValue,
      std::shared_ptr<display::RendererAccessCoordinator::DisplayReservation>
          reservationValue)
      : renderer(rendererValue), activeFlags(activeFlagsValue),
        reservation(std::move(reservationValue)) {}

  bool synchronize(std::uint32_t flags, std::string &errorMessage) override {
    if (!renderer.synchronize(flags, errorMessage)) {
      return false;
    }
    activeFlags = flags;
    return true;
  }

private:
  RendererSpy &renderer;
  std::uint32_t &activeFlags;
  std::shared_ptr<display::RendererAccessCoordinator::DisplayReservation>
      reservation;
};

display::SDLDisplayBackend
makeBackend(const std::shared_ptr<FakeSDLAdapter> &adapter,
            RendererSpy &renderer, std::uint32_t &activeFlags) {
  return display::SDLDisplayBackend(
      adapter, false, [&activeFlags]() { return activeFlags; },
      [&renderer, &activeFlags](std::uint32_t flags, std::string &errorMessage)
          -> std::unique_ptr<display::IRendererDisplayTransaction> {
        if (!renderer.reserve(flags, errorMessage)) {
          return nullptr;
        }
        class UpdatingTransaction final
            : public display::IRendererDisplayTransaction {
        public:
          UpdatingTransaction(RendererSpy &rendererValue,
                              std::uint32_t &activeFlagsValue)
              : renderer(rendererValue), activeFlags(activeFlagsValue) {}
          bool synchronize(std::uint32_t candidateFlags,
                           std::string &syncError) override {
            if (!renderer.synchronize(candidateFlags, syncError)) {
              return false;
            }
            activeFlags = candidateFlags;
            return true;
          }

        private:
          RendererSpy &renderer;
          std::uint32_t &activeFlags;
        };
        return std::make_unique<UpdatingTransaction>(renderer, activeFlags);
      });
}

display::SDLDisplayBackend
makeCoordinatedBackend(const std::shared_ptr<FakeSDLAdapter> &adapter,
                       RendererSpy &renderer, std::uint32_t &activeFlags,
                       display::RendererAccessCoordinator &coordinator,
                       std::function<void()> onReserved = {}) {
  return display::SDLDisplayBackend(
      adapter, false, [&activeFlags]() { return activeFlags; },
      [&renderer, &activeFlags, &coordinator,
       onReserved = std::move(onReserved)](std::uint32_t flags,
                                           std::string &errorMessage)
          -> std::unique_ptr<display::IRendererDisplayTransaction> {
        ++renderer.reservationCalls;
        renderer.lastFlags = flags;
        auto reservation = coordinator.tryAcquireDisplay(errorMessage);
        if (!reservation.has_value()) {
          return nullptr;
        }
        if (onReserved) {
          onReserved();
        }
        auto lifetime = std::make_shared<
            display::RendererAccessCoordinator::DisplayReservation>(
            std::move(*reservation));
        return std::make_unique<CoordinatedRendererTransaction>(
            renderer, activeFlags, std::move(lifetime));
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
  require(renderer.reservationCalls == 1 && renderer.synchronizeCalls == 1,
          "display mutation synchronizes even when VSync flags are unchanged");
  require(renderer.lastFlags == 0x40,
          "unchanged non-VSync renderer flags are preserved");
  require(backend.restore(previous, errorMessage) ==
              display::RestoreStatus::Restored,
          "display restore succeeds with unchanged renderer flags");
  require(renderer.reservationCalls == 2 && renderer.synchronizeCalls == 2,
          "actual restore synchronizes even when VSync flags are unchanged");
}

void testReservationRejectsBeforeAnySDLMutation() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  renderer.reservationSucceeds = false;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  std::string errorMessage;

  require(!backend.apply(resizedWindow(), errorMessage),
          "export reservation rejects the display transaction");
  require(adapter->fullscreenCalls == 0 && adapter->clearModeCalls == 0 &&
              adapter->sizeCalls == 0 && adapter->positionCalls == 0,
          "reservation failure leaves SDL untouched");
  require(renderer.synchronizeCalls == 0,
          "reservation failure never enters renderer synchronization");
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
  require(renderer.reservationCalls == 1 && renderer.synchronizeCalls == 1,
          "one reservation owns candidate mutation and atomic SDL repair");
}

void testExclusiveVerificationRejectsAcceptedLowerRefresh() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  adapter->currentPhysicalModes[0] =
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
  adapter->state.requestedWindowMode =
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

void testExclusiveRestoreUsesCapturedRequestedModeNotPhysicalQuery() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  adapter->state.mode = DisplayMode::ExclusiveFullscreen;
  adapter->state.width = 1920;
  adapter->state.height = 1080;
  adapter->state.requestedWindowMode =
      display::SDLNativeDisplayMode{1920, 1080, 60, 7};
  adapter->currentPhysicalModes[0] =
      display::SDLNativeDisplayMode{1920, 1080, 60, 7};
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  const auto previous = backend.capture();
  std::string errorMessage;

  require(backend.apply(VideoSettings{}, errorMessage),
          "exclusive runtime can preview windowed mode");
  require(backend.restore(previous, errorMessage) ==
              display::RestoreStatus::Restored,
          "exclusive requested mode restores successfully");
  require(adapter->lastRequestedMode.has_value() &&
              adapter->lastRequestedMode->refreshRateHz == 60 &&
              adapter->lastRequestedMode->pixelFormat == 7,
          "restore uses captured requested mode separately from physical mode");
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

  require(backend.restore(previous, errorMessage) ==
              display::RestoreStatus::Failed,
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
      [&renderer](std::uint32_t flags, std::string &errorMessage)
          -> std::unique_ptr<display::IRendererDisplayTransaction> {
        if (!renderer.reserve(flags, errorMessage)) {
          return nullptr;
        }
        return std::make_unique<FakeRendererTransaction>(renderer);
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

void testRendererReservationExcludesExportBeforeFirstSDLMutation() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  renderer.failNextSynchronize = true;
  std::uint32_t activeFlags = 0x40;
  std::mutex rendererMutex;
  std::atomic<bool> exportActive{false};
  display::RendererAccessCoordinator coordinator(rendererMutex, exportActive);
  std::latch reservationAcquired{1};
  std::latch exportAttempted{1};
  std::atomic<bool> exportEntered{false};
  std::atomic<bool> mutationSawExport{false};

  auto backend = makeCoordinatedBackend(
      adapter, renderer, activeFlags, coordinator,
      [&reservationAcquired]() { reservationAcquired.count_down(); });
  adapter->beforeFirstMutation = [&]() { exportAttempted.wait(); };
  adapter->mutationObserver = [&]() {
    if (exportActive.load(std::memory_order_acquire) ||
        exportEntered.load(std::memory_order_acquire)) {
      mutationSawExport.store(true, std::memory_order_release);
    }
  };
  std::thread exporter([&]() {
    reservationAcquired.wait();
    exportAttempted.count_down();
    auto exportReservation = coordinator.acquireExport();
    exportEntered.store(true, std::memory_order_release);
  });

  std::string errorMessage;
  require(!backend.apply(resizedWindow(), errorMessage),
          "injected renderer failure repairs SDL while export waits");
  exporter.join();
  require(!mutationSawExport.load(std::memory_order_acquire),
          "export cannot enter during candidate mutation or SDL repair");
  require(exportEntered.load(std::memory_order_acquire),
          "export proceeds after the display reservation releases");
}

void testExportUiFrameUnlockStillExcludesDisplayTransactions() {
  std::mutex rendererMutex;
  std::atomic<bool> exportActive{false};
  display::RendererAccessCoordinator coordinator(rendererMutex, exportActive);
  auto exportReservation = coordinator.acquireExport();
  require(exportActive.load(std::memory_order_acquire) &&
              exportReservation.ownsLock(),
          "export publishes active only after owning renderer access");

  exportReservation.unlockForUiFrame();
  std::string errorMessage;
  require(!coordinator.tryAcquireDisplay(errorMessage).has_value(),
          "display reservation stays excluded during an export UI-frame gap");
  exportReservation.relockAfterUiFrame();
  exportReservation.release();
  require(!exportActive.load(std::memory_order_acquire) &&
              coordinator.tryAcquireDisplay(errorMessage).has_value(),
          "display reservation resumes after export release");
}

void testExportRequestPublishesBeforeWaitingForRendererAccess() {
  std::mutex rendererMutex;
  std::atomic<bool> exportActive{false};
  display::RendererAccessCoordinator coordinator(rendererMutex, exportActive);
  std::latch requestStarted{1};
  std::latch releaseExport{1};
  std::atomic<bool> exportEntered{false};

  std::unique_lock<std::mutex> mainRender(rendererMutex);
  std::thread exporter([&]() {
    requestStarted.count_down();
    auto exportReservation = coordinator.acquireExport();
    exportEntered.store(true, std::memory_order_release);
    releaseExport.wait();
  });

  requestStarted.wait();
  for (int attempt = 0;
       attempt < 100 && !coordinator.exportRequested(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(coordinator.exportRequested() &&
              !exportActive.load(std::memory_order_acquire) &&
              !exportEntered.load(std::memory_order_acquire),
          "an export request is visible before it can acquire the renderer");

  mainRender.unlock();
  for (int attempt = 0;
       attempt < 100 && !exportEntered.load(std::memory_order_acquire);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(exportEntered.load(std::memory_order_acquire) &&
              exportActive.load(std::memory_order_acquire),
          "yielding the renderer lets the requested export acquire it");

  releaseExport.count_down();
  exporter.join();
  require(!coordinator.exportRequested() &&
              !exportActive.load(std::memory_order_acquire),
          "releasing the final export clears its pending handoff");
}

void testOverlappingExportUiFrameUnlocksRemainReferenceSafe() {
  std::mutex rendererMutex;
  std::atomic<bool> exportActive{false};
  display::RendererAccessCoordinator coordinator(rendererMutex, exportActive);
  auto firstExport = coordinator.acquireExport();
  firstExport.unlockForUiFrame();

  auto secondExport = coordinator.acquireExport();
  secondExport.release();

  std::string errorMessage;
  require(exportActive.load(std::memory_order_acquire),
          "a completed overlapping export cannot clear the older lifetime");
  require(!coordinator.tryAcquireDisplay(errorMessage).has_value(),
          "display stays excluded until every export lifetime ends");

  firstExport.release();
  require(!exportActive.load(std::memory_order_acquire) &&
              coordinator.tryAcquireDisplay(errorMessage).has_value(),
          "the final export release reopens display transactions");
}

void testBorderlessPreviewConfirmsAtDesktopDimensions() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  FramePacer pacer;
  display::DisplaySettingsManager manager(backend, pacer, VideoSettings{});
  auto candidate = VideoSettings{};
  candidate.mode = DisplayMode::BorderlessFullscreen;

  const auto preview =
      manager.beginPreview(candidate, std::chrono::steady_clock::time_point{});
  require(preview.status == display::ApplyStatus::PreviewPending &&
              preview.effective.width == 1920 &&
              preview.effective.height == 1080,
          "SDL applies borderless preview at the selected desktop bounds");
  const auto confirmed = manager.confirmPreview();
  require(confirmed.status == display::ApplyStatus::Applied &&
              !manager.hasPendingPreview(),
          "desktop-sized borderless runtime confirms requested window intent");
}

void testConcreteTransientRollbackRetriesForCancelFocusAndTimeout() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  std::mutex rendererMutex;
  std::atomic<bool> exportActive{false};
  display::RendererAccessCoordinator coordinator(rendererMutex, exportActive);
  auto backend =
      makeCoordinatedBackend(adapter, renderer, activeFlags, coordinator);
  FramePacer pacer;
  display::DisplaySettingsManager manager(backend, pacer, VideoSettings{});
  auto candidate = VideoSettings{};
  candidate.width = 1920;
  candidate.height = 1080;
  candidate.frameCap = 120;
  const auto now = std::chrono::steady_clock::time_point{};

  manager.beginPreview(candidate, now);
  std::latch cancelExportHeld{1};
  std::latch releaseCancelExport{1};
  std::thread cancelExporter([&]() {
    auto exportReservation = coordinator.acquireExport();
    cancelExportHeld.count_down();
    releaseCancelExport.wait();
  });
  cancelExportHeld.wait();
  const auto deferredCancel =
      manager.cancelPreview(display::RollbackReason::Cancelled);
  require(
      deferredCancel.status == display::ApplyStatus::RollbackPending &&
          manager.hasPendingPreview() && pacer.currentFrameCap() == 120,
      "export-active cancel keeps the concrete preview pending and coherent");
  releaseCancelExport.count_down();
  cancelExporter.join();
  require(manager.cancelPreview(display::RollbackReason::Cancelled).status ==
              display::ApplyStatus::Applied,
          "cancel retries after export releases renderer access");

  manager.beginPreview(candidate, now);
  std::latch busyRendererHeld{1};
  std::latch releaseBusyRenderer{1};
  std::atomic<bool> busyRendererAcquired{false};
  std::thread busyRenderer([&]() {
    std::string busyError;
    auto busyReservation = coordinator.tryAcquireDisplay(busyError);
    busyRendererAcquired.store(busyReservation.has_value(),
                               std::memory_order_release);
    busyRendererHeld.count_down();
    if (busyReservation.has_value()) {
      releaseBusyRenderer.wait();
    }
  });
  busyRendererHeld.wait();
  require(busyRendererAcquired.load(std::memory_order_acquire),
          "test acquires a temporary competing renderer reservation");
  const auto deferredFocus = manager.onFocusLost();
  require(deferredFocus.has_value() &&
              deferredFocus->status == display::ApplyStatus::RollbackPending &&
              manager.hasPendingPreview(),
          "busy-renderer focus rollback remains pending");
  releaseBusyRenderer.count_down();
  busyRenderer.join();
  require(manager.tick(now).has_value() && !manager.hasPendingPreview(),
          "focus rollback retries on the next manager tick");

  manager.beginPreview(candidate, now);
  std::latch timeoutExportHeld{1};
  std::latch releaseTimeoutExport{1};
  std::thread timeoutExporter([&]() {
    auto timeoutExport = coordinator.acquireExport();
    timeoutExportHeld.count_down();
    releaseTimeoutExport.wait();
  });
  timeoutExportHeld.wait();
  const auto deferredTimeout =
      manager.tick(now + display::DisplaySettingsManager::kConfirmationTimeout);
  require(deferredTimeout.has_value() &&
              deferredTimeout->status ==
                  display::ApplyStatus::RollbackPending &&
              manager.hasPendingPreview(),
          "export-active timeout remains pending");
  releaseTimeoutExport.count_down();
  timeoutExporter.join();
  const auto completedTimeout =
      manager.tick(now + display::DisplaySettingsManager::kConfirmationTimeout +
                   std::chrono::seconds(1));
  require(completedTimeout.has_value() &&
              completedTimeout->status == display::ApplyStatus::Applied &&
              !manager.hasPendingPreview(),
          "timeout retries after export releases renderer access");
}

void testMaximizedWindowStateRestoresAndVerifies() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  adapter->state.maximized = true;
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  const auto previous = backend.capture();
  auto candidate = VideoSettings{};
  candidate.mode = DisplayMode::BorderlessFullscreen;
  std::string errorMessage;

  require(backend.apply(candidate, errorMessage),
          "maximized window can enter a display preview");
  require(!adapter->state.maximized,
          "display mutation leaves maximized window state explicitly");
  require(backend.restore(previous, errorMessage) ==
              display::RestoreStatus::Restored,
          "captured maximized state restores successfully");
  require(adapter->state.maximized && adapter->maximizeCalls >= 2,
          "maximized state is restored and verified through the adapter");

  adapter->ignoreMaximize = false;
  require(backend.apply(candidate, errorMessage),
          "second maximized-state preview starts");
  adapter->ignoreMaximize = true;
  require(backend.restore(previous, errorMessage) ==
              display::RestoreStatus::Failed,
          "restore rejects a window manager that ignores maximization");
}

void testMaximizedRollbackPreservesNormalWindowGeometry() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);

  const auto normal = backend.capture();
  adapter->restoredWindowState = adapter->state;
  adapter->setWindowMaximized(true);
  const auto maximized = backend.capture();
  require(maximized.windowMaximized &&
              maximized.settings.width == normal.settings.width &&
              maximized.settings.height == normal.settings.height,
          "maximized capture retains its underlying normal window size");

  auto candidate = VideoSettings{};
  candidate.mode = DisplayMode::BorderlessFullscreen;
  std::string errorMessage;
  require(backend.apply(candidate, errorMessage),
          "maximized geometry regression enters a borderless preview");
  require(backend.restore(maximized, errorMessage) ==
              display::RestoreStatus::Restored,
          "maximized geometry regression restores its snapshot");

  adapter->setWindowMaximized(false);
  require(adapter->state.width == normal.settings.width &&
              adapter->state.height == normal.settings.height &&
              adapter->state.x == normal.windowX &&
              adapter->state.y == normal.windowY,
          "unmaximizing after rollback returns to the original normal bounds");
}

void testNormalResizeRefreshesFutureMaximizedRestoreGeometry() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  FramePacer pacer;
  display::DisplaySettingsManager manager(backend, pacer, VideoSettings{});

  adapter->state.width = 1600;
  adapter->state.height = 900;
  adapter->state.x = 140;
  adapter->state.y = 160;
  adapter->restoredWindowState = adapter->state;
  require(!manager.tick(std::chrono::steady_clock::time_point{}).has_value(),
          "idle display tick only observes current normal geometry");

  adapter->setWindowMaximized(true);
  const auto maximized = backend.capture();
  require(maximized.windowMaximized && maximized.settings.width == 1600 &&
              maximized.settings.height == 900 && maximized.windowX == 140 &&
              maximized.windowY == 160,
          "later normal resize becomes the maximized restore rectangle");
}

void testMaximizedPreviewRollbackRestoresCachedNormalGeometry() {
  auto adapter = std::make_shared<FakeSDLAdapter>();
  adapter->modes[0].push_back({1600, 900, 60, 1});
  RendererSpy renderer;
  std::uint32_t activeFlags = 0x40;
  auto backend = makeBackend(adapter, renderer, activeFlags);
  FramePacer pacer;
  display::DisplaySettingsManager manager(backend, pacer, VideoSettings{});

  adapter->restoredWindowState = adapter->state;
  adapter->setWindowMaximized(true);
  auto candidate = VideoSettings{};
  candidate.width = 1600;
  candidate.height = 900;
  require(
      manager.beginPreview(candidate, std::chrono::steady_clock::time_point{})
              .status == display::ApplyStatus::PreviewPending,
      "maximized window can preview a different normal resolution");

  const auto cancelled =
      manager.cancelPreview(display::RollbackReason::Cancelled);
  require(cancelled.status == display::ApplyStatus::Applied &&
              cancelled.effective.width == 1280 &&
              cancelled.effective.height == 720,
          "rollback reports the restored pre-maximize normal geometry");
  require(backend.capture().settings.width == 1280 &&
              backend.capture().settings.height == 720,
          "rollback repairs the cached normal geometry as well as SDL");
}
} // namespace

int main() {
  testDisplayMutationSynchronizesRendererWithUnchangedFlags();
  testReservationRejectsBeforeAnySDLMutation();
  testRendererFailureCanRestoreSDLAndRendererCoherently();
  testManagerRollsConcreteSDLTransactionBackAfterRendererFailure();
  testExclusiveVerificationRejectsAcceptedLowerRefresh();
  testVsyncOnlyExclusiveChangeVerifiesTheEffectiveMode();
  testExclusiveRestoreUsesCapturedRequestedModeNotPhysicalQuery();
  testWindowedRestoreRejectsIgnoredPosition();
  testCapabilitiesUseTheSDLAdapterAndDeduplicateModes();
  testFixedMobileDisplayOnlyAdvertisesFrameCap();
  testRendererReservationExcludesExportBeforeFirstSDLMutation();
  testExportUiFrameUnlockStillExcludesDisplayTransactions();
  testExportRequestPublishesBeforeWaitingForRendererAccess();
  testOverlappingExportUiFrameUnlocksRemainReferenceSafe();
  testBorderlessPreviewConfirmsAtDesktopDimensions();
  testMaximizedRollbackPreservesNormalWindowGeometry();
  testNormalResizeRefreshesFutureMaximizedRestoreGeometry();
  testMaximizedPreviewRollbackRestoresCachedNormalGeometry();
  testConcreteTransientRollbackRetriesForCancelFocusAndTimeout();
  testMaximizedWindowStateRestoresAndVerifies();
  return 0;
}
