#include "skin/GameplaySkinActivationRequest.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/package/SkinPathPolicy.h"
#include "music_select/MusicSelectLaunchPolicy.h"

#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) { ++failures; std::cerr << message << '\n'; }
}
}
namespace skin {
struct MusicSelectSkinSessionPreparationContext {
  SkinStorageRoots storageRoots;
  int &resourcePreparation;
  int initialFrame;
  int builtinImageReader;
  int audioBackend;
  int initialLegacyInputGeneration;
  std::stop_token stop;
};
struct MusicSelectSkinSession {
  inline static std::atomic_uint64_t serial = 0;
  std::uint64_t epoch = ++serial;
  std::uint64_t frame = 0;
  GameplaySkinActivationRequest request;
  std::unique_ptr<LuaSkinRuntime> runtime;
  LuaCallbackId tick;
  explicit MusicSelectSkinSession(GameplaySkinActivationRequest value,
                                  const SkinStorageRoots &roots)
      : request(std::move(value)) {
    auto files = LuaSkinFileSystem::create({
        .revision = request.activation.revision.readView(),
        .entry = request.activation.entry,
        .storageRoots = roots,
        .profileId = request.profileId,
        .allowDataWrites = false});
    if (files.failure) std::cerr << files.failure->message << '\n';
    auto created = LuaSkinRuntime::create({.purpose = LuaRuntimePurpose::MusicSelect,
                                          .fileSystem = std::move(files.fileSystem)});
    if (created.failure) std::cerr << created.failure->message << '\n';
    runtime = std::move(created.runtime);
    assert(runtime);
    assert(runtime->loadHeader().value);
    auto configured = runtime->loadConfigured({});
    assert(configured.value);
    auto callback = configured.value->callbackNamed("tick");
    assert(callback);
    tick = *callback;
    assert(runtime->enterRenderPhase().ok);
  }
  void resumeAudio() { runtime->resumeAudio(); }
  void suspendAudio() { runtime->suspendAudio(); }
  double counter() {
    assert(runtime->beginFrame(++frame).ok);
    auto result = runtime->invoke(tick, {});
    assert(result.value);
    if (const auto *integer = std::get_if<std::int64_t>(&*result.value)) return *integer;
    return std::get<double>(*result.value);
  }
  static std::unique_ptr<MusicSelectSkinSession> prepare(
      GameplaySkinActivationRequest request, MusicSelectSkinSessionPreparationContext context) {
    return std::make_unique<MusicSelectSkinSession>(std::move(request), context.storageRoots);
  }
};
struct AudioLimits {
  std::size_t maximumIdentities, maximumEncodedBytes, maximumDecodedBytes;
};
int createLuaSkinApplicationAudioBackend(int, std::function<float()>, AudioLimits,
                                        const std::shared_ptr<int> &) { return 0; }
}
namespace rendering { constexpr int render_width = 1280, render_height = 720; }
namespace archive_file { constexpr int readFileBounded = 0; }
struct SceneReturnTarget {
  void *retained = nullptr;
  static SceneReturnTarget Retained(void *value) { return {value}; }
  static SceneReturnTarget Registered(const char *) { return {}; }
};
enum class SettingsDestination { Profile };
struct SettingsScene {
  SceneReturnTarget target;
  SettingsScene(auto &, SettingsDestination, SceneReturnTarget value) : target(value) {}
};
struct SceneManager {
  void *retained = nullptr;
  std::string registered;
  void changeScene(std::unique_ptr<SettingsScene> settings, bool = false) { retained = settings->target.retained; }
  void changeScene(const char *name) { registered = name; }
};
struct Lifecycle {
  std::optional<skin::GameplaySkinActivationRequest> next;
  skin::GameplaySkinAcquisitionDisposition disposition = skin::GameplaySkinAcquisitionDisposition::Ready;
  skin::GameplaySkinAcquisition acquireForSkinType(int type, bool gameplay) {
    assert(type == 5 && !gameplay);
    return {.disposition = disposition, .request = std::move(next)};
  }
};
struct ApplicationContext {
  struct {
    skin::SkinProfileSettings skin;
    struct { struct { float masterVolume = 1; } audio; } audioVideo;
  } settings;
  std::optional<skin::SkinStorageRoots> skinStorageRoots;
  int resources = 0;
  int *skinResourcePreparationService = &resources;
  std::shared_ptr<int> skinLiveResourceCounters = std::make_shared<int>();
  struct { int legacyInputGeneration(int, int) { return 0; } } inputDeviceRegistry;
  struct { int audioRuntime() { return 0; } } jukebox;
  std::atomic_bool appInBackground = false;
  Lifecycle *gameplaySkinLifecycle = nullptr;
  SceneManager *sceneManager = nullptr;
};
struct View { void setVisible(bool) {} };
struct Preview { void resumeDefaultBgm() {} };
struct MusicSelectScene {
  ApplicationContext &context;
  skin::GameplaySkinActivationRequest activationRequest_;
  std::string selectedSkinPath_;
  bool sceneActive_ = true, launching_ = false, failed_ = false;
  bool reactivateSkinOnResume_ = false;
  std::unique_ptr<skin::MusicSelectSkinSession> skinSession_;
  std::future<std::unique_ptr<skin::MusicSelectSkinSession>> skinPreparation_;
  std::stop_source skinPreparationStop_;
  View *errorView_ = nullptr;
  Preview *previewAudio_ = nullptr;
  ACTIVATION_IDENTITY
  MusicSelectScene(ApplicationContext &value, skin::GameplaySkinActivationRequest request)
      : context(value), activationRequest_(std::move(request)) {}
  void init() { INITIAL_ACTIVATION }
  void finishPreparation() {
    if (skinPreparation_.valid()) skinSession_ = skinPreparation_.get();
  }
  int makeFrame() { return 0; }
  void enterError(std::vector<skin::SkinDiagnostic>) { failed_ = true; skinSession_.reset(); }
  void buildSkinLoadingView() {}
  void hideDecideOverlay() {}
  void onApplicationBackgroundChanged(bool) {}
  void syncToolbar() {}
  void reloadLibrary() {}
  void configureSoundServices() {}
  void selectedBarMoved() {}
  void startInputListening() {}
  void cancelSkinPreparation();
  bool activateSkin(skin::GameplaySkinActivationRequest);
  bool reactivateSkinAfterSettings();
  void openSettings();
  void onResume();
};
SCENE_METHODS

int musicSelectSettingsRuntimeTests() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() /
      ("selector-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root / "package");
  const auto source = root / "package/main.luaskin";
  std::ofstream(source) << R"lua(
local sentinel = 0
return {type = 5, w = 1280, h = 720, destination = {}, tick = function()
  sentinel = sentinel + 1
  return sentinel
end}
)lua";
  const auto package = *skin::normalizePackageId("Package").package;
  const auto entry = *skin::normalizeEntryPath(package, "main.luaskin").entry;
  auto lease = skin::SkinRevisionLease::fromLiveSource(
      {package, std::string(64, 'a'), 1, fs::file_size(source)}, root / "package");
  assert(lease);
  std::uint64_t serial = 0;
  const auto request = [&] {
    return skin::GameplaySkinActivationRequest{
        .sessionSerial = ++serial,
        .profileId = {"77777777-7777-4777-8777-777777777777"},
        .activation = {.revision = lease->clone(), .entry = entry,
                       .reconciledSettings = {}, .configurationDigest = "configuration"},
        .viewport = {}, .safetyLevel = skin::SkinSafetyLevel::Standard};
  };
  for (int changed = 0; changed < 9; ++changed) {
    ApplicationContext context;
    context.skinStorageRoots = skin::SkinStorageRoots{
        .visiblePackages = root, .privateRevisions = root / "revisions",
        .privateCatalog = root / "catalog", .profileOverlays = root / "overlays"};
    Lifecycle lifecycle;
    SceneManager manager;
    context.gameplaySkinLifecycle = &lifecycle;
    context.sceneManager = &manager;
    MusicSelectScene scene(context, request());
    scene.init();
    scene.finishPreparation();
    const auto epoch = scene.skinSession_->epoch;
    expect(scene.skinSession_->counter() == 1 && scene.skinSession_->counter() == 2,
           "Lua sentinel must carry custom runtime state before Settings");
    auto next = request();
    if (changed == 1) next.activation.configurationDigest = "changed";
    if (changed == 2) next.activation.reconciledSettings.options["option"] = 1;
    if (changed == 3) next.viewport.translateX = 16;
    if (changed == 4) next.safetyLevel = skin::SkinSafetyLevel::BeatorajaCompatibility;
    if (changed == 5) next.profileId.opaque = "88888888-8888-4888-8888-888888888888";
    if (changed == 6) {
      next.activation.entry = *skin::normalizeEntryPath(package, "other.luaskin").entry;
      fs::copy_file(source, root / "package/other.luaskin", fs::copy_options::overwrite_existing);
    }
    if (changed == 7) {
      next.activation.revision = std::move(*skin::SkinRevisionLease::fromLiveSource(
          {package, std::string(64, 'b'), 1, fs::file_size(source)}, root / "package"));
    }
    if (changed == 8) {
      fs::create_directories(root / "replacement");
      fs::copy_file(source, root / "replacement/main.luaskin", fs::copy_options::overwrite_existing);
      next.activation.revision = std::move(*skin::SkinRevisionLease::fromLiveSource(
          {package, std::string(64, 'a'), 1, fs::file_size(source)}, root / "replacement"));
    }
    lifecycle.next = std::move(next);
    scene.openSettings();
    expect(manager.retained == &scene, "Settings must retain its selector owner");
    scene.sceneActive_ = false;
    scene.skinSession_->suspendAudio();
    scene.onResume();
    scene.finishPreparation();
    if (changed == 0) {
      expect(scene.skinSession_->epoch == epoch && scene.skinSession_->counter() == 3,
             "unchanged Settings must preserve the live Lua runtime and sentinel despite a fresh request serial");
    } else {
      expect(scene.skinSession_->epoch != epoch && scene.skinSession_->counter() == 1,
             "changed activation input must create a fresh configured Lua runtime");
    }
    const auto retainedEpoch = scene.skinSession_->epoch;
    auto &active = scene.skinSession_->request;
    lifecycle.next = skin::GameplaySkinActivationRequest{
        .sessionSerial = ++serial, .profileId = active.profileId,
        .activation = {.revision = active.activation.revision.clone(), .entry = active.activation.entry,
                       .reconciledSettings = active.activation.reconciledSettings,
                       .configurationDigest = active.activation.configurationDigest},
        .viewport = active.viewport, .safetyLevel = active.safetyLevel};
    scene.openSettings();
    scene.onResume();
    scene.finishPreparation();
    expect(scene.skinSession_->epoch == retainedEpoch,
           "unchanged round trip after replacement must retain the replacement runtime");
    lifecycle.disposition = skin::GameplaySkinAcquisitionDisposition::BuiltIn;
    scene.openSettings();
    scene.onResume();
    expect(manager.registered == "MainMenu", "BuiltIn selection must still leave Lua selector");
    lifecycle.disposition = skin::GameplaySkinAcquisitionDisposition::Failed;
    scene.openSettings();
    scene.onResume();
    expect(scene.failed_, "failed acquisition must still enter skin error recovery");
  }
  fs::remove_all(root);
  return failures == 0 ? 0 : 1;
}
