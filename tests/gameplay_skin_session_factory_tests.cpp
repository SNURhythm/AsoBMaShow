#include "scene/play/GameplaySkinSessionFactory.h"
#include "skin/beatoraja/LuaSkinAudioHost.h"
#include "skin/beatoraja/LuaSkinHttpClient.h"

#include "rendering/common.h"
#include "skin/beatoraja/GameplaySkinValidator.h"
#include "skin/beatoraja/SkinDiagnosticHistory.h"
#include "skin/package/SkinPackageCatalog.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace rendering {
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {

int failures = 0;
namespace fs = std::filesystem;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    root_ = fs::temp_directory_path() /
            ("asobmashow-gameplay-skin-session-factory-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++serial));
    fs::create_directories(root_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

skin::SkinEntryId fixtureEntry() {
  const auto package = skin::normalizePackageId("FactorySkin");
  expect(package.package.has_value(), "factory fixture package is valid");
  if (!package.package) {
    std::abort();
  }
  const auto entry =
      skin::normalizeEntryPath(*package.package, "skin/main.luaskin");
  expect(entry.entry.has_value(), "factory fixture entry is valid");
  if (!entry.entry) {
    std::abort();
  }
  return *entry.entry;
}

skin::SkinDiagnostic diagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = "skin/main.luaskin",
          .severity = skin::DiagnosticSeverity::Error,
          .source = skin::SkinSourceLocation{
              .virtualPath = "skin/main.luaskin", .line = 37, .column = 11}};
}

GameplaySkinSessionServices noSelectionServices() {
  return {.acquire = [](int) { return skin::GameplaySkinAcquisition{}; }};
}

GameplaySkinSessionInput validInput() { return {.keyMode = 7}; }

class FactoryTextureDevice final : public skin::SkinTextureDevice {
public:
  FactoryTextureDevice() : owner_(std::this_thread::get_id()) {}

  bgfx::TextureHandle create(const image_decode::DecodedImageData &) override {
    return bgfx::TextureHandle{nextHandle_++};
  }

  void destroy(bgfx::TextureHandle) noexcept override {}

  bool ownsCurrentThread() const noexcept override {
    return std::this_thread::get_id() == owner_;
  }

private:
  std::thread::id owner_;
  std::uint16_t nextHandle_ = 1;
};

struct HistoryFixture {
  TempDirectory temp;
  skin::SkinPackageCatalog catalog{temp.root()};
  skin::SkinDiagnosticHistory history{catalog};
};

class FactoryAudioBackend final : public skin::LuaSkinAudioBackend {
public:
  float systemVolume() const noexcept override { return 1.0F; }
  std::optional<skin::LuaSkinAudioIdentity>
  load(const fs::path &, std::stop_token stop) noexcept override {
    std::unique_lock lock(mutex);
    loadedStop = stop;
    loadEntered = true;
    condition.notify_all();
    std::stop_callback cancellation(stop, [this] { condition.notify_all(); });
    condition.wait(lock, [this, stop] {
      return !blockLoad || stop.stop_requested();
    });
    if (stop.stop_requested()) {
      return std::nullopt;
    }
    return skin::LuaSkinAudioIdentity{.value = 1};
  }
  void play(skin::LuaSkinAudioIdentity, float, bool) noexcept override {}
  void stop(skin::LuaSkinAudioIdentity) noexcept override {}
  void dispose(skin::LuaSkinAudioIdentity) noexcept override {}

  std::stop_token loadedStop;
  bool blockLoad = false;
  bool loadEntered = false;
  std::mutex mutex;
  std::condition_variable condition;

  bool waitUntilLoadEntered() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(2),
                              [this] { return loadEntered; });
  }
};

class FactoryHttpTransport final : public skin::LuaSkinHttpTransport {
public:
  skin::LuaSkinHttpOpenResult open(std::string_view, int,
                                   skin::LuaSkinHttpLimits) override {
    return {.failure = "factory fixture does not perform HTTP"};
  }
};

GameplaySkinSessionServices
failedAcquireServices(HistoryFixture &fixture, skin::SkinDiagnostic failure) {
  const auto entry = fixtureEntry();
  return {.acquire =
              [entry, failure = std::move(failure)](int) mutable {
                return skin::GameplaySkinAcquisition{
                    .disposition =
                        skin::GameplaySkinAcquisitionDisposition::Failed,
                    .failure = skin::GameplaySkinAcquisitionFailure{
                        .entry = entry,
                        .revisionDigest = std::string(64, 'a'),
                        .configurationDigest = std::string(64, 'b'),
                        .diagnostic = std::move(failure)}};
              },
          .diagnosticHistory = &fixture.history};
}

struct ReadyFixture {
  TempDirectory temp;
  skin::SkinStorageRoots roots{.visiblePackages = temp.root() / "visible",
                               .privateRevisions = temp.root() / "revisions",
                               .privateCatalog = temp.root() / "catalog",
                               .profileOverlays = temp.root() / "overlays"};
  skin::SkinPackageCatalog catalog{temp.root() / "catalog"};
  skin::SkinDiagnosticHistory history{catalog};
  skin::SkinResourcePreparationService resources;
  std::shared_ptr<skin::SkinLiveResourceCounters> counters =
      std::make_shared<skin::SkinLiveResourceCounters>();
  std::shared_ptr<FactoryTextureDevice> textureDevice =
      std::make_shared<FactoryTextureDevice>();
  std::shared_ptr<FactoryAudioBackend> audioBackend =
      std::make_shared<FactoryAudioBackend>();
  bool audioBackendForwarded = false;
  bool httpTransportForwarded = false;
  bool httpStopForwarded = false;
  std::stop_token httpStop;
  bool legacyInputCaptureForwarded = false;
  bool exerciseAudioPreparation = false;
  std::optional<skin::LuaSkinLegacyInputGeneration> capturedLegacyInput;
  skin::SkinConfigurationWriteQueue writes;
  std::stop_source stopSource;
  std::stop_token expectedStop;
  PlayfieldChartVisualModel chart;
  PlayfieldVisualState state;
  PlayfieldProjectionResult projection;
  skin::SkinEntryId entry = fixtureEntry();
  skin::SkinProfileId profile =
      *skin::makeSkinProfileId("77777777-7777-4777-8777-777777777777");
  std::optional<skin::SkinRevisionLease> lease;
  skin::SkinValidationResult validation;

  ReadyFixture() {
    fs::create_directories(roots.visiblePackages);
    const fs::path packageRoot =
        roots.visiblePackages / entry.package.directoryName;
    fs::create_directories(packageRoot / "skin");
    std::ofstream(packageRoot / "skin/main.luaskin")
        << "return { type = 0, w = 1280, h = 720 }\n";
    lease = skin::SkinRevisionLease::fromLiveSource(
        {.package = entry.package, .lowercaseSha256 = std::string(64, 'c')},
        packageRoot);
    expect(lease.has_value(), "factory ready fixture acquires live revision");
    if (lease) {
      skin::GameplaySkinValidator validator(resources);
      validation = validator.validate(lease->readView(), entry, nullptr, {});
      expect(validation.reconciledSettings.has_value() &&
                 !validation.configurationDigest.empty(),
             "factory ready fixture validates the selected skin");
    }
    state.clock.serial = 1;
    state.authority.loadingState = PlayfieldLoadingState::Loaded;
    projection.frameSerial = state.clock.serial;
    expectedStop = stopSource.get_token();
  }

  GameplaySkinSessionServices services() {
    auto activation =
        std::make_shared<std::optional<skin::ValidatedSkinActivation>>(
            skin::ValidatedSkinActivation{
                .revision = std::move(*lease),
                .entry = entry,
                .reconciledSettings = *validation.reconciledSettings,
                .configurationDigest = validation.configurationDigest});
    return {.acquire =
                [activation, profile = profile](int) mutable {
                  skin::GameplaySkinAcquisition result;
                  if (!activation->has_value()) {
                    result.disposition =
                        skin::GameplaySkinAcquisitionDisposition::Failed;
                    result.failure = skin::GameplaySkinAcquisitionFailure{
                        .diagnostic = diagnostic("skin.test.activation_reused",
                                                 "activation was reused")};
                    return result;
                  }
                  result.disposition =
                      skin::GameplaySkinAcquisitionDisposition::Ready;
                  result.request = skin::GameplaySkinActivationRequest{
                      .sessionSerial = 71,
                      .profileId = profile,
                      .activation = std::move(**activation),
                      .viewport = {}};
                  activation->reset();
                  return result;
                },
            .storageRoots = &roots,
            .resourcePreparation = &resources,
            .liveResourceCounters = counters,
            .createHttpTransport = [this](std::stop_token stop) {
              httpStop = stop;
              httpStopForwarded = stop.stop_possible() &&
                                  stop == expectedStop;
              return std::make_unique<FactoryHttpTransport>();
            },
            .audioBackend = audioBackend,
            .captureLegacyInputGeneration = [] {
              return skin::LuaSkinLegacyInputGeneration{
                  .drawableWidth = 111, .drawableHeight = 222};
            },
            .configurationWrites = &writes,
            .diagnosticHistory = &history,
            .stop = expectedStop,
            .createSessionForTesting =
                [this, textureDevice = textureDevice](
                    skin::ValidatedSkinActivation activation,
                    skin::PlaySkinSessionContext context) {
                  audioBackendForwarded =
                      context.audioBackend == audioBackend;
                  httpTransportForwarded = context.httpTransport != nullptr;
                  legacyInputCaptureForwarded =
                      static_cast<bool>(context.captureLegacyInputGeneration);
                  if (context.captureLegacyInputGeneration) {
                    capturedLegacyInput =
                        context.captureLegacyInputGeneration();
                  }
                  if (exerciseAudioPreparation && context.audioBackend) {
                    (void)context.audioBackend->load(
                        roots.visiblePackages / "preparation-audio.wav",
                        context.stop);
                  }
                  context.textureDevice = textureDevice;
                  return skin::PlaySkinSession::create(std::move(activation),
                                                       std::move(context));
                }};
  }

  GameplaySkinSessionInput input() const {
    return {
        .keyMode = 7,
        .chartModel = &chart,
        .initialState = &state,
        .initialProjection = &projection,
        .safeUiBounds = {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0}};
  }
};

void factoryKeepsBuiltInPresentationForNoSelection() {
  expect(createGameplaySkinSession(noSelectionServices(), validInput())
                 .disposition == GameplaySkinSessionDisposition::BuiltIn,
         "no selection keeps built-in presentation available");
}

void factoryKeepsBuiltInPresentationWhenAcquisitionIsUnavailable() {
  expect(createGameplaySkinSession({}, validInput()).disposition ==
             GameplaySkinSessionDisposition::BuiltIn,
         "unavailable optional skin acquisition keeps built-in presentation "
         "available");
}

void factoryPreservesLifecycleDiagnosticAndRecordsIt() {
  HistoryFixture fixture;
  const auto failed = createGameplaySkinSession(
      failedAcquireServices(
          fixture,
          diagnostic("skin.lifecycle.activation_unavailable", "not ready")),
      validInput());
  expect(failed.disposition == GameplaySkinSessionDisposition::Failed &&
             failed.failure &&
             failed.failure->diagnostic.code ==
                 "skin.lifecycle.activation_unavailable",
         "factory preserves lifecycle diagnostic");
  const auto records = fixture.history.records();
  expect(!records.empty() &&
             records.back().phase == skin::SkinDiagnosticPhase::Session,
         "factory records failed acquisition");
  expect(!records.empty() && records.back().diagnostic.source &&
             records.back().diagnostic.source->virtualPath ==
                 "skin/main.luaskin" &&
             records.back().diagnostic.source->line == 37 &&
             records.back().diagnostic.source->column == 11 &&
             records.back().diagnostic.severity ==
                 skin::DiagnosticSeverity::Error &&
             records.back().entry == fixtureEntry(),
         "factory preserves diagnostic source, severity, and session identity");
}

void factoryTransfersTheOwningSessionExactlyOnce() {
  ReadyFixture fixture;
  if (!fixture.lease) {
    return;
  }
  const auto ready =
      createGameplaySkinSession(fixture.services(), fixture.input());
  expect(ready.disposition == GameplaySkinSessionDisposition::Ready &&
             ready.session != nullptr && fixture.audioBackendForwarded &&
             fixture.httpTransportForwarded && fixture.httpStopForwarded &&
             fixture.legacyInputCaptureForwarded &&
             fixture.capturedLegacyInput &&
             fixture.capturedLegacyInput->drawableWidth == 111 &&
             fixture.capturedLegacyInput->drawableHeight == 222,
         "factory transfers the owning session, cancellable HTTP transport, "
         "narrow audio backend, and legacy-input capture exactly once");
}

void factorySuppliesTheValueOnlyProductionLegacyInputCapture() {
  ReadyFixture fixture;
  if (!fixture.lease) {
    return;
  }
  auto services = fixture.services();
  services.captureLegacyInputGeneration = {};
  const auto ready =
      createGameplaySkinSession(std::move(services), fixture.input());
  expect(ready.disposition == GameplaySkinSessionDisposition::Ready &&
             ready.session != nullptr && fixture.capturedLegacyInput &&
             fixture.capturedLegacyInput->drawableWidth ==
                 rendering::render_width &&
             fixture.capturedLegacyInput->drawableHeight ==
                 rendering::render_height,
         "factory defaults to a value-only production drawable/input/controller snapshot");
}

void factoryOwnerCancellationReachesHttpAndAudioPreparation() {
  ReadyFixture fixture;
  if (!fixture.lease) {
    return;
  }
  GameplaySkinSessionStopOwner owner;
  fixture.expectedStop = owner.token();
  fixture.exerciseAudioPreparation = true;
  const auto ready =
      createGameplaySkinSession(fixture.services(), fixture.input());
  expect(ready.disposition == GameplaySkinSessionDisposition::Ready &&
             ready.session != nullptr && fixture.httpStopForwarded &&
             fixture.audioBackend->loadedStop.stop_possible() &&
             !fixture.audioBackend->loadedStop.stop_requested(),
         "production stop owner reaches HTTP construction and audio preparation");
  owner.requestStop();
  expect(fixture.audioBackend->loadedStop.stop_requested(),
         "cancelling through the session owner reaches prepared audio work");
}

void productionOwnerCancelsBlockedPreparationBeforeSessionPublication() {
  ReadyFixture fixture;
  if (!fixture.lease) {
    return;
  }
  GameplaySkinSessionStopOwner owner;
  const auto cancellation = owner.handle();
  fixture.expectedStop = owner.token();
  fixture.exerciseAudioPreparation = true;
  fixture.audioBackend->blockLoad = true;

  auto result = std::async(std::launch::async, [&fixture] {
    return createGameplaySkinSession(fixture.services(), fixture.input());
  });
  expect(fixture.audioBackend->waitUntilLoadEntered(),
         "production owner fixture reaches blocking audio preparation");
  cancellation.requestStop();
  const bool returnedBeforeRelease =
      result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  if (!returnedBeforeRelease) {
    fixture.audioBackend->blockLoad = false;
    fixture.audioBackend->condition.notify_all();
  }
  auto cancelled = result.get();
  expect(returnedBeforeRelease && fixture.httpStopForwarded &&
             fixture.httpStop.stop_requested() &&
             fixture.audioBackend->loadedStop.stop_requested(),
         "externally reachable production owner cancels blocked HTTP/audio work");
  expect(cancelled.disposition == GameplaySkinSessionDisposition::Failed &&
             cancelled.session == nullptr,
         "cancelled synchronous preparation never publishes a gameplay session");
}

void replayOwnerForwardsExternalCancellationDuringBlockedPreparation() {
  ReadyFixture fixture;
  if (!fixture.lease) {
    return;
  }
  std::stop_source exportCancellation;
  GameplaySkinSessionStopOwner owner(exportCancellation.get_token());
  fixture.expectedStop = owner.token();
  fixture.exerciseAudioPreparation = true;
  fixture.audioBackend->blockLoad = true;

  auto result = std::async(std::launch::async, [&fixture] {
    return createGameplaySkinSession(fixture.services(), fixture.input());
  });
  expect(fixture.audioBackend->waitUntilLoadEntered(),
         "replay owner fixture reaches blocking audio preparation");
  exportCancellation.request_stop();
  const bool returnedBeforeRelease =
      result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  if (!returnedBeforeRelease) {
    fixture.audioBackend->blockLoad = false;
    fixture.audioBackend->condition.notify_all();
  }
  auto cancelled = result.get();
  expect(returnedBeforeRelease && fixture.httpStopForwarded &&
             fixture.httpStop.stop_requested() &&
             fixture.audioBackend->loadedStop.stop_requested() &&
             cancelled.disposition == GameplaySkinSessionDisposition::Failed &&
             cancelled.session == nullptr,
         "replay export cancellation reaches HTTP/audio preparation and "
         "prevents session publication");
}

} // namespace

int main() {
  factoryKeepsBuiltInPresentationForNoSelection();
  factoryKeepsBuiltInPresentationWhenAcquisitionIsUnavailable();
  factoryPreservesLifecycleDiagnosticAndRecordsIt();
  factoryTransfersTheOwningSessionExactlyOnce();
  factorySuppliesTheValueOnlyProductionLegacyInputCapture();
  factoryOwnerCancellationReachesHttpAndAudioPreparation();
  productionOwnerCancelsBlockedPreparationBeforeSessionPublication();
  replayOwnerForwardsExternalCancellationDuringBlockedPreparation();
  return failures == 0 ? 0 : 1;
}
