#include "scene/play/GameplaySkinSessionFactory.h"
#include "skin/beatoraja/LuaSkinAudioHost.h"

#include "rendering/common.h"
#include "skin/beatoraja/GameplaySkinValidator.h"
#include "skin/beatoraja/SkinDiagnosticHistory.h"
#include "skin/package/SkinPackageCatalog.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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
  load(const fs::path &) noexcept override {
    return std::nullopt;
  }
  void play(skin::LuaSkinAudioIdentity, float, bool) noexcept override {}
  void stop(skin::LuaSkinAudioIdentity) noexcept override {}
  void dispose(skin::LuaSkinAudioIdentity) noexcept override {}
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
  bool legacyInputCaptureForwarded = false;
  std::optional<skin::LuaSkinLegacyInputSnapshot> capturedLegacyInput;
  skin::SkinConfigurationWriteQueue writes;
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
            .configurationWrites = &writes,
            .diagnosticHistory = &history,
            .audioBackend = audioBackend,
            .captureLegacyInputSnapshot = [] {
              return skin::LuaSkinLegacyInputSnapshot{
                  .drawableWidth = 111, .drawableHeight = 222};
            },
            .createSessionForTesting =
                [this, textureDevice = textureDevice](
                    skin::ValidatedSkinActivation activation,
                    skin::PlaySkinSessionContext context) {
                  audioBackendForwarded =
                      context.audioBackend == audioBackend;
                  legacyInputCaptureForwarded =
                      static_cast<bool>(context.captureLegacyInputSnapshot);
                  if (context.captureLegacyInputSnapshot) {
                    capturedLegacyInput = context.captureLegacyInputSnapshot();
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
             fixture.legacyInputCaptureForwarded &&
             fixture.capturedLegacyInput &&
             fixture.capturedLegacyInput->drawableWidth == 111 &&
             fixture.capturedLegacyInput->drawableHeight == 222,
         "factory transfers the owning session and narrow audio backend "
         "plus legacy-input capture exactly once");
}

void factorySuppliesTheValueOnlyProductionLegacyInputCapture() {
  ReadyFixture fixture;
  if (!fixture.lease) {
    return;
  }
  auto services = fixture.services();
  services.captureLegacyInputSnapshot = {};
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

} // namespace

int main() {
  factoryKeepsBuiltInPresentationForNoSelection();
  factoryKeepsBuiltInPresentationWhenAcquisitionIsUnavailable();
  factoryPreservesLifecycleDiagnosticAndRecordsIt();
  factoryTransfersTheOwningSessionExactlyOnce();
  factorySuppliesTheValueOnlyProductionLegacyInputCapture();
  return failures == 0 ? 0 : 1;
}
