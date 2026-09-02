#include "skin/beatoraja/LuaSkinRuntime.h"

#include "music_select_runtime_ledger_assertions.h"

#include "skin/SkinProfileSettings.h"
#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinHostModules.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <atomic>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace skin;

#ifndef ASOBMASHOW_SOURCE_DIR
#define ASOBMASHOW_SOURCE_DIR "."
#endif

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string readFixture(std::string_view relativePath) {
  std::ifstream input(std::string(ASOBMASHOW_SOURCE_DIR) + "/" +
                      std::string(relativePath));
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string readText(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-lua-runtime-test-" + std::to_string(++serial));
    } while (!fs::create_directory(root_));
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class AcceptFiles final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

SkinStorageRoots rootsBelow(const fs::path &root) {
  return {.visiblePackages = root / "visible",
          .privateRevisions = root / "revisions",
          .privateCatalog = root / "catalog",
          .profileOverlays = root / "overlays"};
}

SkinProfileId profileFor(std::uint64_t serial) {
  std::ostringstream id;
  id << "99999999-9999-4999-8999-";
  id.width(12);
  id.fill('0');
  id << serial;
  return *makeSkinProfileId(id.str());
}

class RuntimePackageFixture {
public:
  RuntimePackageFixture()
      : roots(rootsBelow(temp.root())),
        package(*normalizePackageId("RuntimeContract").package) {
    const fs::path committed = fs::path(ASOBMASHOW_SOURCE_DIR) /
                               "tests/fixtures/beatoraja_skin/packages/"
                               "runtime_contract";
    const fs::path source = temp.root() / "source";
    fs::copy(committed, source, fs::copy_options::recursive);
    const fs::path visible = roots.visiblePackages / package.directoryName;
    fs::create_directories(visible.parent_path());
    fs::copy(source, visible, fs::copy_options::recursive);

    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto snapshot = snapshotter.snapshot(source, package, {}, {});
    expect(snapshot.prepared.has_value(), "runtime package snapshots");
    if (snapshot.prepared) {
      prepared.emplace(std::move(*snapshot.prepared));
    }
  }

  SkinEntryId entry(std::string_view fixture) const {
    return *normalizeEntryPath(package, "skin/" + std::string(fixture)).entry;
  }

  SkinEntryId rootEntry(std::string_view fixture) const {
    return *normalizeEntryPath(package, fixture).entry;
  }

  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageId package;
  AcceptFiles aliases;
  std::optional<PreparedSkinRevision> prepared;
};

RuntimePackageFixture &runtimePackage() {
  static RuntimePackageFixture fixture;
  return fixture;
}

void copyLuaSources(const fs::path &source, const fs::path &destination) {
  for (const fs::directory_entry &entry :
       fs::recursive_directory_iterator(source)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const fs::path extension = entry.path().extension();
    if (extension != ".lua" && extension != ".luaskin") {
      continue;
    }
    const fs::path target =
        destination / entry.path().lexically_relative(source);
    fs::create_directories(target.parent_path());
    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
  }
}

struct RuntimeHarness {
  std::unique_ptr<LuaSkinRuntime> runtime;
  LuaSkinFileSystem *fileSystem = nullptr;
  fs::path overlayRoot;
};

std::unique_ptr<RuntimeHarness>
makeHarness(LuaRuntimePurpose purpose, std::string_view entryFixture,
            bool forceWriteCapableFileSystem = false,
            bool entryAtPackageRoot = false,
            SkinSafetyLevel safetyLevel = SkinSafetyLevel::Standard,
            bool preservePinnedLuaSandbox = false) {
  static std::atomic_uint64_t profileSerial{0};
  RuntimePackageFixture &package = runtimePackage();
  if (!package.prepared) {
    return {};
  }
  const SkinEntryId entry = entryAtPackageRoot
                                ? package.rootEntry(entryFixture)
                                : package.entry(entryFixture);
  const bool writes =
      purpose != LuaRuntimePurpose::Catalog || forceWriteCapableFileSystem;
  const std::optional<SkinProfileId> profile =
      writes ? std::optional<SkinProfileId>(profileFor(++profileSerial))
             : std::nullopt;
  auto fileSystem =
      LuaSkinFileSystem::create({.revision = package.prepared->readView(),
                                 .entry = entry,
                                 .storageRoots = package.roots,
                                 .profileId = profile,
                                 .allowDataWrites = writes,
                                 .safetyPolicy =
                                     SkinSafetyPolicy(
                                         safetyLevel,
                                         std::numeric_limits<std::uint64_t>::max(),
                                         preservePinnedLuaSandbox)});
  expect(fileSystem.fileSystem != nullptr,
         "runtime filesystem is created for the fixture");
  if (!fileSystem.fileSystem) {
    return {};
  }
  LuaSkinFileSystem *fileSystemPointer = fileSystem.fileSystem.get();
  fs::path overlay;
  if (profile) {
    const auto derived =
        deriveSkinPrivateOverlayRoot(package.roots, *profile, entry);
    expect(derived.root.has_value(), "runtime overlay identity derives");
    if (derived.root) {
      overlay = *derived.root;
    }
  }
  auto created = LuaSkinRuntime::create(
      {.purpose = purpose,
       .fileSystem = std::move(fileSystem.fileSystem),
       .safetyPolicy = SkinSafetyPolicy(
           safetyLevel, std::numeric_limits<std::uint64_t>::max(),
           preservePinnedLuaSandbox)});
  expect(created.runtime != nullptr && !created.failure,
         "bounded Lua runtime is created");
  if (!created.runtime) {
    return {};
  }
  return std::make_unique<RuntimeHarness>(RuntimeHarness{
      .runtime = std::move(created.runtime),
      .fileSystem = fileSystemPointer,
      .overlayRoot = std::move(overlay),
  });
}

LuaCallbackId requireCallback(LuaValueHandle &value, std::string_view name) {
  const auto callback = value.callbackNamed(name);
  expect(callback.has_value(), "named fixture callback is retained by runtime");
  return callback.value_or(LuaCallbackId{});
}

struct ReentrantEventExecutorContext {
  LuaSkinRuntime *runtime = nullptr;
  LuaCallbackId callback;
  std::optional<SkinDiagnostic> nestedFailure;
};

LuaSkinEventExecutionResult executeReentrantEvent(
    void *opaque, int, std::span<const int>) noexcept {
  auto &context = *static_cast<ReentrantEventExecutorContext *>(opaque);
  const std::array<LuaScalar, 1> arguments{LuaScalar{0.5}};
  const auto nested = context.runtime->invoke(context.callback, arguments);
  context.nestedFailure = nested.failure;
  return {.failure = nested.failure};
}

void loadThroughConfigured(RuntimeHarness &harness,
                           BeatorajaSkinConfiguration configuration = {}) {
  expect(harness.runtime->loadHeader().value.has_value(),
         "fixture header phase succeeds");
  expect(harness.runtime->loadConfigured(configuration).value.has_value(),
         "fixture configured phase succeeds");
}

void testPurposeSpecificBudgetsAreFixed() {
  constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
  expect(LuaRuntimePolicy::catalogLoad.maxAllocatorBytes == 32 * mebibyte,
         "catalog allocator budget is 32 MiB");
  expect(LuaRuntimePolicy::catalogLoad.maxInstructions == 2'000'000,
         "catalog execution budget is 2,000,000 instructions");
  expect(LuaRuntimePolicy::catalogLoad.maxWallTime.count() == 2'000,
         "catalog execution deadline is 2 seconds");
  expect(LuaRuntimePolicy::validationAndGameplayLoad.maxAllocatorBytes ==
             128 * mebibyte,
         "validation/gameplay allocator budget is 128 MiB");
  expect(LuaRuntimePolicy::validationAndGameplayLoad.maxInstructions ==
             20'000'000,
         "validation/gameplay phase budget is 20,000,000 instructions");
  expect(LuaRuntimePolicy::validationAndGameplayLoad.maxWallTime.count() ==
             10'000,
         "validation/gameplay phase deadline is 10 seconds");
  expect(LuaRuntimePolicy::gameplayCallback.maxInstructions == 250'000,
         "one callback gets 250,000 instructions");
  expect(LuaRuntimePolicy::gameplayCallback.maxWallTime.count() == 4,
         "one callback gets 4 milliseconds");
  expect(LuaRuntimePolicy::gameplayFrame.maxInstructions == 1'000'000,
         "one frame gets 1,000,000 callback instructions");
  expect(LuaRuntimePolicy::gameplayFrame.maxWallTime.count() == 6,
         "one frame gets 6 milliseconds of callback wall time");
}

void testRuntimeContractsUseSourceAuthoritiesAndProvenance() {
  const auto manifest =
      readFixture("tests/fixtures/beatoraja_skin/reference_manifest.json");
  const auto legacyTrace = readFixture(
      "tests/fixtures/beatoraja_skin/traces/legacy_lua_upstream_v1.json");
  const auto provenance = readFixture("tests/fixtures/beatoraja_skin/packages/"
                                      "runtime_contract/provenance.json");

  expect(!manifest.empty() && !legacyTrace.empty() && !provenance.empty(),
         "runtime consumes committed audit, trace, and provenance");
  expect(manifest.find("\"schemaVersion\": 2") != std::string::npos,
         "runtime fixture uses the schema-v2 acceptance contract");
  expect(manifest.find("ordinaryRuntimeIo") != std::string::npos,
         "runtime fixture records ordinary selected-root I/O");
  expect(manifest.find("negativeScenarios") == std::string::npos,
         "runtime fixture does not preserve a synthetic render-I/O denial");
  expect(provenance.find("c2ed5db1a46145ed10790c3872f717e95b59db9d") !=
             std::string::npos,
         "runtime fixtures record the pinned source commit");
}

void testFilesystemReadsTheSelectedEntryWithoutAHostPath() {
  RuntimePackageFixture &package = runtimePackage();
  const SkinEntryId entry = package.entry("two_phase.luaskin");
  auto fileSystem =
      LuaSkinFileSystem::create({.revision = package.prepared->readView(),
                                 .entry = entry,
                                 .storageRoots = package.roots});
  expect(fileSystem.fileSystem != nullptr, "direct entry fixture creates");
  if (!fileSystem.fileSystem) {
    return;
  }
  const auto read = fileSystem.fileSystem->readEntry(1024 * 1024);
  expect(
      !read.failure && !read.bytes.empty(),
      "direct entry read uses the selected identity, not a caller host path");
}

void testCatalogEntrySourceIsBoundedBeforeHostAllocation() {
  RuntimePackageFixture fixture;
  if (!fixture.prepared) {
    return;
  }
  const SkinEntryId entry = fixture.entry("two_phase.luaskin");
  const fs::path visibleEntry =
      fixture.roots.visiblePackages / fixture.package.directoryName /
      "skin/two_phase.luaskin";
  std::error_code resizeError;
  fs::resize_file(visibleEntry,
                  LuaRuntimePolicy::catalogLoad.maxAllocatorBytes + 1,
                  resizeError);
  expect(!resizeError, "oversized visible entry fixture is created sparsely");
  if (resizeError) {
    return;
  }

  auto fileSystem = LuaSkinFileSystem::create(
      {.revision = fixture.prepared->readView(),
       .entry = entry,
       .storageRoots = fixture.roots,
       .allowDataWrites = false});
  expect(fileSystem.fileSystem != nullptr,
         "catalog runtime filesystem is created for an oversized entry");
  if (!fileSystem.fileSystem) {
    return;
  }
  auto runtime = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::Catalog,
       .fileSystem = std::move(fileSystem.fileSystem)});
  expect(runtime.runtime != nullptr,
         "catalog runtime is created before entry compilation");
  if (!runtime.runtime) {
    return;
  }

  const auto header = runtime.runtime->loadHeader();
  expect(header.failure &&
             header.failure->code == "skin_lua_host_limit_exceeded",
         "catalog entry source is rejected before host allocation exceeds its "
         "Lua budget");
}

void testUnrestrictedRuntimeLiftsSourceBudgetWithoutChangingSafeOs() {
  RuntimePackageFixture fixture;
  if (!fixture.prepared) {
    return;
  }

  const SkinEntryId oversizedEntry = fixture.entry("two_phase.luaskin");
  const fs::path oversizedPath = fixture.roots.visiblePackages /
                                 fixture.package.directoryName /
                                 "skin/two_phase.luaskin";
  std::ofstream oversized(oversizedPath, std::ios::binary | std::ios::trunc);
  constexpr std::size_t mebibyte = 1024ULL * 1024ULL;
  const std::string whitespace(mebibyte, ' ');
  oversized << "-- unrestricted runtime source budget fixture\n";
  for (std::size_t index = 0;
       index <= LuaRuntimePolicy::catalogLoad.maxAllocatorBytes / mebibyte;
       ++index) {
    oversized.write(whitespace.data(),
                    static_cast<std::streamsize>(whitespace.size()));
  }
  oversized << "\nreturn {}\n";
  oversized.close();

  auto files = LuaSkinFileSystem::create(
      {.revision = fixture.prepared->readView(),
       .entry = oversizedEntry,
       .storageRoots = fixture.roots,
       .safetyPolicy = SkinSafetyPolicy(SkinSafetyLevel::Unrestricted)});
  expect(files.fileSystem != nullptr,
         "unrestricted runtime filesystem creates for oversized source");
  if (!files.fileSystem) {
    return;
  }
  auto runtime = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::Catalog,
       .fileSystem = std::move(files.fileSystem),
       .safetyPolicy = SkinSafetyPolicy(SkinSafetyLevel::Unrestricted)});
  expect(runtime.runtime != nullptr,
         "unrestricted runtime creates for oversized source");
  if (!runtime.runtime) {
    return;
  }
  expect(runtime.runtime->loadHeader().value.has_value(),
         "Unrestricted loads a valid source beyond the Standard host budget");

  RuntimePackageFixture &package = runtimePackage();
  const fs::path globalsPath = package.roots.visiblePackages /
                               package.package.directoryName /
                               "skin/unrestricted_globals.luaskin";
  writeText(globalsPath,
            "return { query = function() return os == nil and 'nil' or "
            "type(os.setlocale) end }\n");
  auto standard = makeHarness(LuaRuntimePurpose::Gameplay,
                              "unrestricted_globals.luaskin");
  auto unrestricted = makeHarness(
      LuaRuntimePurpose::Gameplay, "unrestricted_globals.luaskin", false,
      false, SkinSafetyLevel::Unrestricted);
  auto musicSelect = makeHarness(
      LuaRuntimePurpose::MusicSelect, "unrestricted_globals.luaskin", false,
      false, SkinSafetyLevel::Unrestricted);
  if (!standard || !unrestricted || !musicSelect) {
    return;
  }
  for (const auto &[name, harness, expected] :
       std::array<std::tuple<std::string_view, RuntimeHarness *, std::string_view>,
                  3>{{{"Standard", standard.get(), "function"},
                       {"Unrestricted", unrestricted.get(), "function"},
                       {"Music select", musicSelect.get(), "function"}}}) {
    auto header = harness->runtime->loadHeader();
    expect(header.value.has_value(), "process-global fixture header loads");
    if (!header.value) {
      continue;
    }
    const auto callback = header.value->callbackNamed("query");
    expect(callback && harness->runtime->loadConfigured({}).value.has_value() &&
               harness->runtime->enterRenderPhase().ok &&
               harness->runtime->beginFrame(1).ok,
           "process-global fixture enters a callback frame");
    if (!callback) {
      continue;
    }
    const auto result = harness->runtime->invoke(*callback, {});
    expect(result.value && std::get<std::string>(*result.value) == expected,
           std::string(name) +
               " preserves Beatoraja SafeOsLib's standard os surface");
  }
}

void testCatalogLuaLoadersBoundSourceBeforeHostAllocation() {
  RuntimePackageFixture fixture;
  if (!fixture.prepared) {
    return;
  }
  const fs::path skinRoot = fixture.roots.visiblePackages /
                            fixture.package.directoryName / "skin";
  writeText(skinRoot / "oversized_dofile.luaskin",
            "return dofile('oversized.lua')\n");
  writeText(skinRoot / "oversized_require.luaskin",
            "return require('oversized')\n");
  const fs::path oversized = skinRoot / "oversized.lua";
  writeText(oversized, "return {}\n");
  std::error_code resizeError;
  fs::resize_file(oversized,
                  LuaRuntimePolicy::catalogLoad.maxAllocatorBytes + 1,
                  resizeError);
  expect(!resizeError,
         "oversized Lua loader fixture is created sparsely in the package");
  if (resizeError) {
    return;
  }

  for (const std::string_view entryName : {"oversized_dofile.luaskin",
                                            "oversized_require.luaskin"}) {
    auto fileSystem = LuaSkinFileSystem::create(
        {.revision = fixture.prepared->readView(),
         .entry = fixture.entry(entryName),
         .storageRoots = fixture.roots,
         .allowDataWrites = false});
    expect(fileSystem.fileSystem != nullptr,
           "catalog runtime filesystem is created for an oversized Lua load");
    if (!fileSystem.fileSystem) {
      continue;
    }
    auto runtime = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Catalog,
         .fileSystem = std::move(fileSystem.fileSystem)});
    expect(runtime.runtime != nullptr,
           "catalog runtime is created before dynamic Lua loading");
    if (!runtime.runtime) {
      continue;
    }
    const auto header = runtime.runtime->loadHeader();
    expect(header.failure &&
               header.failure->code == "skin_lua_host_limit_exceeded",
           "catalog Lua file loaders reject source before host allocation "
           "exceeds the Lua budget");
  }
}

void testStrictTwoPhaseStateMachineUsesOneState() {
  auto harness = makeHarness(LuaRuntimePurpose::Gameplay, "two_phase.luaskin");
  if (!harness) {
    return;
  }
  const auto premature = harness->runtime->loadConfigured({});
  expect(!premature.value && premature.failure,
         "configured phase cannot run before header phase");
  expect(harness->runtime->phase() == LuaRuntimePhase::Created,
         "failed configured transition preserves Created");
  expect(harness->runtime->loadHeader().value.has_value(),
         "nil-skin_config header succeeds");
  expect(harness->runtime->phase() == LuaRuntimePhase::HeaderLoaded,
         "header advances exactly one phase");
  expect(harness->runtime->loadConfigured({}).value.has_value(),
         "same-state configured execution observes header globals/module");
  expect(harness->runtime->phase() == LuaRuntimePhase::Configured,
         "configured advances exactly one phase");
  expect(harness->runtime->enterRenderPhase().ok,
         "clean gameplay runtime enters render");
  expect(harness->runtime->phase() == LuaRuntimePhase::Render,
         "render transition is terminal");
  expect(!harness->runtime->loadConfigured({}).value,
         "configured execution cannot repeat in render phase");
}

void testMainStateAccessorsOpenOnlyAtRenderTransition() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Gameplay, "render_main_state.luaskin");
  if (!harness) {
    return;
  }
  auto header = harness->runtime->loadHeader();
  expect(header.value.has_value(),
         "render main-state fixture sees an empty header module");
  if (!header.value) {
    return;
  }
  const LuaCallbackId callback =
      requireCallback(*header.value, "render_main_state_ready");
  expect(harness->runtime->loadConfigured({}).value.has_value(),
         "configured entry receives main-state accessors on the header table");
  expect(harness->runtime->enterRenderPhase().ok &&
             harness->runtime->beginFrame(1).ok,
         "configured main-state accessors remain usable in render");
  const auto result = harness->runtime->invoke(callback, {});
  const auto *ready = result.value ? std::get_if<bool>(&*result.value) : nullptr;
  expect(ready != nullptr && *ready && !result.failure,
         "the header-captured main-state table is populated in place");
}

void testMusicSelectPurposeHasTheConfiguredLiveHost() {
  auto harness =
      makeHarness(LuaRuntimePurpose::MusicSelect, "render_main_state.luaskin");
  if (!harness) {
    return;
  }
  auto header = harness->runtime->loadHeader();
  expect(header.value.has_value(),
         "music-select live host executes its header pass");
  if (!header.value) {
    return;
  }
  const LuaCallbackId callback =
      requireCallback(*header.value, "render_main_state_ready");
  expect(harness->runtime->loadConfigured({}).value.has_value() &&
             harness->runtime->enterRenderPhase().ok &&
             harness->runtime->beginFrame(1).ok,
         "music-select purpose enters the configured render host");
  const auto result = harness->runtime->invoke(callback, {});
  const auto *ready = result.value ? std::get_if<bool>(&*result.value) : nullptr;
  expect(ready && *ready && !result.failure,
         "music-select exposes main_state, timers, events, and callbacks");

  auto legacy =
      makeHarness(LuaRuntimePurpose::MusicSelect, "legacy_facade.luaskin");
  expect(legacy && legacy->runtime->loadHeader().value.has_value(),
         "music-select purpose exposes the pinned legacy facade");
}

void testRuntimeProvidesBeatorajaSafeOsLibrary() {
  for (const auto purpose : {LuaRuntimePurpose::Catalog,
                             LuaRuntimePurpose::Validation,
                             LuaRuntimePurpose::Gameplay}) {
    auto harness = makeHarness(purpose, "os_compatibility.luaskin");
    if (!harness) {
      continue;
    }
    expect(harness->runtime->loadHeader().value.has_value(),
           "Beatoraja-compatible os.time succeeds in every runtime purpose");
  }
}

void testRuntimeSearchesVirtualPackagePath() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Validation, "package_path_module.luaskin");
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "a skin package.path module root is searched virtually");
}

void testRuntimeSearchesSkinPrefixedPackageRoot() {
  auto harness = makeHarness(LuaRuntimePurpose::Validation,
                             "root_package_path_module.luaskin");
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "a skin-prefixed package.path searches the package root");
}

void testRuntimeInitialPackagePathNamesSelectedDirectory() {
  auto harness = makeHarness(LuaRuntimePurpose::Validation,
                             "nested/initial_package_path.luaskin");
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "initial package.path names the selected virtual directory");
}

void testRuntimeInitialPackagePathNamesPackageRoot() {
  auto harness = makeHarness(LuaRuntimePurpose::Validation,
                             "initial_root_package_path.luaskin", false, true);
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "initial package.path names the virtual package root");
}

void testRuntimeDiagnosesVirtualModuleCandidates() {
  auto harness = makeHarness(LuaRuntimePurpose::Validation,
                             "missing_package_path_module.luaskin");
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "missing virtual modules report the package.path candidates searched");
}

void testRuntimeCreatesConfiguredDynamicHistoryData() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Validation, "dynamic_history.luaskin");
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "dynamic-history fixture loads its header");
  const auto configured = harness->runtime->loadConfigured({});
  expect(configured.value.has_value() && !configured.failure,
         "skin_config.get_path supports a dynamically created history file");
  const fs::path history =
      harness->fileSystem->skinDirectory() / "History/260805/history.txt";
  expect(readText(history) == "record\n",
         "configured get_path data writes stay in the selected skin directory");
}

void testLuaWritesAreVisibleBeforeTheHandleCloses() {
  auto harness = makeHarness(LuaRuntimePurpose::Validation,
                             "direct_io_visibility.luaskin");
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "direct-I/O visibility fixture loads its header");
  const auto configured = harness->runtime->loadConfigured({});
  expect(configured.value.has_value() && !configured.failure,
         "Lua writes are visible through a second handle before close");
}

void testRequestedExternalLuaSkinHeader() {
  const char *configuredRoot = std::getenv("ASOBMASHOW_EXTERNAL_LUA_SKIN_ROOT");
  if (configuredRoot == nullptr || *configuredRoot == '\0') {
    return;
  }
  const fs::path source(configuredRoot);
  expect(fs::is_directory(source),
         "requested external Lua skin root is a readable directory");
  if (!fs::is_directory(source)) {
    return;
  }
  const char *configuredEntry =
      std::getenv("ASOBMASHOW_EXTERNAL_LUA_SKIN_ENTRY");
  const std::string entryPath =
      configuredEntry != nullptr && *configuredEntry != '\0'
          ? configuredEntry
          : "result.luaskin";

  TempDirectory temp;
  const fs::path projectedSource = temp.root() / "source";
  copyLuaSources(source, projectedSource);
  SkinStorageRoots roots = rootsBelow(temp.root());
  const auto package = normalizePackageId(source.filename().string()).package;
  const auto entry = package ? normalizeEntryPath(*package, entryPath).entry
                             : std::nullopt;
  expect(package.has_value() && entry.has_value(),
         "requested external Lua entry has a portable virtual identity");
  if (!package || !entry) {
    return;
  }

  // Exercise the same layout used by an unarchived Beatoraja distribution:
  // the selected package is one visible folder and shared Hub modules are its
  // sibling under Skins.  Runtime execution must not fall back to the private
  // snapshot created for catalog identity.
  fs::create_directories(roots.visiblePackages);
  copyLuaSources(source, roots.visiblePackages / package->directoryName);
  const fs::path sharedHub = source.parent_path() / "Hub";
  if (fs::is_directory(sharedHub)) {
    copyLuaSources(sharedHub, roots.visiblePackages / "Hub");
  }

  AcceptFiles aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto snapshot = snapshotter.snapshot(projectedSource, *package, {}, {});
  expect(snapshot.prepared.has_value(),
         "requested external Lua source projection snapshots");
  if (!snapshot.prepared) {
    return;
  }
  auto fileSystem = LuaSkinFileSystem::create(
      {.revision = snapshot.prepared->readView(),
       .entry = *entry,
       .storageRoots = roots,
       .profileId = profileFor(1'000'000),
       .allowDataWrites = true});
  expect(fileSystem.fileSystem != nullptr,
         "requested external Lua filesystem is created");
  if (!fileSystem.fileSystem) {
    return;
  }
  auto runtime = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::Validation,
       .fileSystem = std::move(fileSystem.fileSystem)});
  expect(runtime.runtime != nullptr,
         "requested external Lua runtime is created");
  if (!runtime.runtime) {
    return;
  }
  const auto header = runtime.runtime->loadHeader();
  if (header.failure) {
    std::cerr << "external Lua header diagnostic: " << header.failure->code
              << ": " << header.failure->message << '\n';
  }
  expect(header.value.has_value() && !header.failure,
         "requested external Lua skin header loads through the virtual package");
}

void testConfiguredTableUsesCanonicalVirtualData() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Validation, "configuration_table.luaskin");
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "configuration fixture header succeeds");
  BeatorajaSkinConfiguration configuration;
  configuration.orderedOptions = {{.name = "Lane type", .value = 927},
                                  {.name = "Gauge", .value = 11}};
  configuration.options = {{"Gauge", 11}, {"Lane type", 927}};
  configuration.enabledOptionIds = {11, 927};
  configuration.filePaths = {{"Background", "images/bg.png"}};
  configuration.offsets = {
      {"Notes offset", {.x = 1, .y = 2, .w = 3, .h = 4, .r = 5, .a = 6}}};
  expect(harness->runtime->loadConfigured(configuration).value.has_value(),
         "configured Lua table matches canonical option/file/offset shape");
}

void testFreshPurposesDoNotShareLuaState() {
  for (const auto purpose :
       {LuaRuntimePurpose::Catalog, LuaRuntimePurpose::Validation,
        LuaRuntimePurpose::Gameplay}) {
    auto first = makeHarness(purpose, "fresh_state.luaskin");
    auto second = makeHarness(purpose, "fresh_state.luaskin");
    if (!first || !second) {
      continue;
    }
    expect(first->runtime->loadHeader().value.has_value() &&
               second->runtime->loadHeader().value.has_value(),
           "fresh runtimes observe no globals or package.loaded from peers");
  }
}

void testValueHandlesLoseAuthorityWhenTheirRuntimeCloses() {
  std::optional<LuaValueHandle> escaped;
  {
    auto harness =
        makeHarness(LuaRuntimePurpose::Validation, "two_phase.luaskin");
    if (!harness) {
      return;
    }
    auto header = harness->runtime->loadHeader();
    expect(header.value.has_value(), "lifetime fixture header succeeds");
    if (!header.value) {
      return;
    }
    escaped.emplace(std::move(*header.value));
  }
  expect(!escaped->callbackNamed("anything").has_value(),
         "value handles retain no Lua authority after runtime destruction");
  escaped.reset();
}

void testProtectedLuaApiAllocationBoundaries() {
  {
    auto harness = makeHarness(LuaRuntimePurpose::Validation, "fresh_state.luaskin");
    if (harness) {
      LuaRuntimeTestHooks::failNextAllocationAt(
          LuaRuntimeTestAllocationPoint::ValueReference);
      const auto result = harness->runtime->loadHeader();
      expect(!result.value && result.failure &&
                 result.failure->code == "skin_lua_allocator_limit_exceeded",
             "value-reference quota failure is returned rather than panicking");
    }
  }

  {
    auto harness = makeHarness(LuaRuntimePurpose::Validation, "callback_wall_time.luaskin");
    if (harness) {
      auto header = harness->runtime->loadHeader();
      expect(header.value.has_value(), "callback allocation fixture loads");
      if (header.value) {
        LuaRuntimeTestHooks::failNextAllocationAt(
            LuaRuntimeTestAllocationPoint::CallbackName);
        const auto nameFailure = header.value->lookupCallbackNamed("host_heavy_callback");
        expect(!nameFailure.callback && nameFailure.failure &&
                   nameFailure.failure->code == "skin_lua_allocator_limit_exceeded",
               "callback-name quota failure is typed and non-fatal");

        LuaRuntimeTestHooks::failNextAllocationAt(
            LuaRuntimeTestAllocationPoint::CallbackReference);
        const auto referenceFailure = header.value->lookupCallbackNamed("host_heavy_callback");
        expect(!referenceFailure.callback && referenceFailure.failure &&
                   referenceFailure.failure->code == "skin_lua_allocator_limit_exceeded",
               "callback-reference quota failure is typed without a leaked slot");
        const auto callback = header.value->callbackNamed("host_heavy_callback");
        expect(callback && callback->slot == 1,
               "failed callback lookups leave no retained callback reference");
      }
    }
  }

  {
    auto harness = makeHarness(LuaRuntimePurpose::Gameplay, "callback_wall_time.luaskin");
    if (harness) {
      auto header = harness->runtime->loadHeader();
      expect(header.value.has_value(), "invoke allocation fixture loads");
      if (header.value) {
        const auto callback = header.value->callbackNamed("host_heavy_callback");
        expect(callback.has_value(), "invoke allocation callback is retained");
        expect(harness->runtime->loadConfigured({}).value.has_value() &&
                   harness->runtime->enterRenderPhase().ok &&
                   harness->runtime->beginFrame(1).ok,
               "invoke allocation fixture reaches a callback frame");
        LuaRuntimeTestHooks::failNextAllocationAt(
            LuaRuntimeTestAllocationPoint::InvokeArgument);
        const std::array<LuaScalar, 1> arguments{
            LuaScalar{std::string(128, 'x')}};
        const auto result = harness->runtime->invoke(*callback, arguments);
        expect(!result.value && result.failure &&
                   result.failure->code == "skin_lua_allocator_limit_exceeded",
               "invoke argument quota failure is typed rather than panicking");
      }
    }
  }
}

void testLanguageSurfaceBit32AndTextOnlyLoading() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Validation, "language_surface.luaskin");
  if (!harness) {
    return;
  }
  loadThroughConfigured(*harness);

  auto forbidden = makeHarness(LuaRuntimePurpose::Validation,
                               "forbidden_capabilities.luaskin");
  if (forbidden) {
    expect(forbidden->runtime->loadHeader().value.has_value(),
           "network, native, process, and unrestricted modules stay absent");
  }

  auto binaryEntry = makeHarness(LuaRuntimePurpose::Validation, "binary.lua");
  if (binaryEntry) {
    const auto result = binaryEntry->runtime->loadHeader();
    expect(!result.value && result.failure,
           "binary entry chunks are rejected before execution");
  }
}

void testCatalogHasNoOverlayWriteOrEventAuthority() {
  auto harness = makeHarness(LuaRuntimePurpose::Catalog,
                             "catalog_read_only.luaskin", true);
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "catalog exposes empty compatibility modules but no write authority");
}

void testLoadQuotasInterruptMemoryStackTablesAndLoops() {
  for (const auto fixture :
       {"allocator_exhaustion.luaskin", "infinite_header.luaskin"}) {
    auto harness = makeHarness(LuaRuntimePurpose::Catalog, fixture);
    if (!harness) {
      continue;
    }
    const auto result = harness->runtime->loadHeader();
    expect(!result.value && result.failure,
           "catalog allocator/instruction/deadline probe is interrupted");
  }

  for (const auto fixture : {"deep_table.luaskin", "table_entries.luaskin",
                             "stack_exhaustion.luaskin"}) {
    auto harness = makeHarness(LuaRuntimePurpose::Validation, fixture);
    if (!harness) {
      continue;
    }
    const auto result = harness->runtime->loadHeader();
    expect(!result.value && result.failure,
           "returned table/stack limit fails deterministically");
  }

  auto configured =
      makeHarness(LuaRuntimePurpose::Validation, "infinite_configured.luaskin");
  if (configured) {
    expect(configured->runtime->loadHeader().value.has_value(),
           "configured-loop fixture header succeeds");
    const auto result = configured->runtime->loadConfigured({});
    expect(!result.value && result.failure,
           "configured execution has its own instruction/deadline budget");
  }

  auto wall =
      makeHarness(LuaRuntimePurpose::Catalog, "wall_time_header.luaskin");
  if (wall) {
    const auto result = wall->runtime->loadHeader();
    expect(result.failure &&
               result.failure->code == "skin_lua_wall_time_limit_exceeded",
           "header work dominated by host calls is stopped by wall time");
  }
}

void testIoFacadeCallShapesHandlesAndHostByteLimit() {
  auto ioHarness =
      makeHarness(LuaRuntimePurpose::Validation, "io_contract.luaskin");
  if (!ioHarness) {
    return;
  }
  loadThroughConfigured(*ioHarness);
  expect(readText(ioHarness->overlayRoot / "skin/fresh/deep/output.txt") ==
             "alpha:7:tail",
         "w/a handles atomically commit chainable writes into the overlay");

  auto limitHarness =
      makeHarness(LuaRuntimePurpose::Validation, "host_limit.luaskin");
  if (limitHarness) {
    expect(limitHarness->runtime->loadHeader().value.has_value(),
           "host data-read byte limit rejects oversized input without effect");
  }
  auto aggregateHarness =
      makeHarness(LuaRuntimePurpose::Validation, "aggregate_handle_limit.luaskin");
  if (aggregateHarness) {
    const auto result = aggregateHarness->runtime->loadHeader();
    expect(!result.value && result.failure,
           "open read handles cannot exceed the aggregate host buffer budget");
  }
}

void testLoadfileUsesBeatorajaRestrictedIoRoot() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Validation, "io_contract.luaskin");
  if (!harness) {
    return;
  }
  const auto header = harness->runtime->loadHeader();
  if (header.failure) {
    std::cerr << "loadfile contract diagnostic: " << header.failure->code
              << ": " << header.failure->message << '\n';
  }
  expect(header.value.has_value(),
         "loadfile compiles a selected-skin Lua file without executing it");
}

void testClosedLegacyFacadeIsExactAndDiagnosed() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Validation, "legacy_facade.luaskin");
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "exact File/Gdx/Input/Controller legacy surface executes");
  expect(fs::is_directory(harness->overlayRoot / "skin/legacy-created"),
         "legacy mkdir creates only an overlay directory");

  std::size_t denied = 0;
  std::size_t urlDenied = 0;
  std::size_t fileMemberDenied = 0;
  for (const auto &entry : harness->runtime->compatibilityDiagnostics()) {
    if (entry.diagnostic.code == "skin_legacy_lua_access_denied") {
      ++denied;
      if (entry.objectId == "java.net.URL") {
        ++urlDenied;
      }
      if (entry.objectId == "java.io.File.member") {
        ++fileMemberDenied;
      }
    }
  }
  expect(denied >= 4,
         "every unaudited class/constructor shape produces a diagnostic");
  expect(urlDenied == 1,
         "repeated legacy denial is deduplicated by denied authority");
  expect(fileMemberDenied == 1,
         "repeated unaudited File members produce one deduplicated denial");
}

void testCoroutineLoopsShareCallbackAndFrameHooks() {
  for (const auto callbackName : {"created_loop", "wrapped_loop"}) {
    auto harness =
        makeHarness(LuaRuntimePurpose::Gameplay, "coroutine_loops.luaskin");
    if (!harness) {
      continue;
    }
    auto header = harness->runtime->loadHeader();
    expect(header.value.has_value(), "coroutine fixture header succeeds");
    if (!header.value) {
      continue;
    }
    const LuaCallbackId callback = requireCallback(*header.value, callbackName);
    expect(harness->runtime->loadConfigured({}).value.has_value(),
           "coroutine fixture configures");
    expect(harness->runtime->enterRenderPhase().ok &&
               harness->runtime->beginFrame(1).ok,
           "coroutine callback frame begins");
    const auto result = harness->runtime->invoke(callback, {});
    expect(!result.value && result.failure,
           "create/wrap child loop consumes the shared hooked callback budget");
  }
}

void testCallbackWallTimeIncludesHostCalls() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Gameplay, "callback_wall_time.luaskin");
  if (!harness) {
    return;
  }
  auto header = harness->runtime->loadHeader();
  if (!header.value) {
    expect(false, "callback wall-time fixture header succeeds");
    return;
  }
  const LuaCallbackId callback =
      requireCallback(*header.value, "host_heavy_callback");
  expect(harness->runtime->loadConfigured({}).value.has_value() &&
             harness->runtime->enterRenderPhase().ok &&
             harness->runtime->beginFrame(1).ok,
         "callback wall-time fixture enters render");
  const auto result = harness->runtime->invoke(callback, {});
  expect(result.failure &&
             result.failure->code == "skin_lua_wall_time_limit_exceeded",
         "callback wall time includes time spent inside host functions");
}

void testCallbackResultStringsUseTheFixedHostLimit() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Gameplay, "callback_result_limit.luaskin");
  if (!harness) {
    return;
  }
  auto header = harness->runtime->loadHeader();
  if (!header.value) {
    expect(false, "callback result-limit fixture header succeeds");
    return;
  }
  const LuaCallbackId callback =
      requireCallback(*header.value, "oversized_result");
  expect(harness->runtime->loadConfigured({}).value.has_value() &&
             harness->runtime->enterRenderPhase().ok &&
             harness->runtime->beginFrame(1).ok,
         "callback result-limit fixture enters render");
  const auto result = harness->runtime->invoke(callback, {});
  expect(result.failure &&
             result.failure->code == "skin_lua_callback_result_invalid",
         "callback strings cannot bypass the fixed host byte limit");
}

void testFrameTotalsResetOnlyForNewVisualState() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Gameplay, "frame_budget.luaskin");
  if (!harness) {
    return;
  }
  auto header = harness->runtime->loadHeader();
  if (!header.value) {
    expect(false, "frame fixture header succeeds");
    return;
  }
  const LuaCallbackId callback = requireCallback(*header.value, "bounded_work");
  expect(harness->runtime->loadConfigured({}).value.has_value() &&
             harness->runtime->enterRenderPhase().ok &&
             harness->runtime->beginFrame(7).ok,
         "frame-budget fixture enters first frame");
  bool exhausted = false;
  for (int invocation = 0; invocation < 32; ++invocation) {
    if (harness->runtime->invoke(callback, {}).failure) {
      exhausted = true;
      break;
    }
  }
  expect(exhausted, "callbacks cumulatively exhaust the frame budget");
  expect(!harness->runtime->beginFrame(7).ok,
         "same visual-state sequence cannot reset totals");
  expect(harness->runtime->beginFrame(8).ok,
         "new visual-state sequence resets totals once");
  expect(!harness->runtime->invoke(callback, {}).failure,
         "bounded callback runs after the next frame reset");
  expect(harness->runtime->invoke({.slot = 999, .generation = 999}, {})
             .failure.has_value(),
         "forged/stale callback IDs are rejected");
}

void testCallbackReentrancyIsRejectedWithoutResettingBudgets() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Gameplay, "writer_transaction.luaskin");
  if (!harness) {
    return;
  }
  auto header = harness->runtime->loadHeader();
  if (!header.value) {
    expect(false, "writer transaction fixture header succeeds");
    return;
  }
  const LuaCallbackId callback =
      requireCallback(*header.value, "reentrant_writer");
  expect(harness->runtime->loadConfigured({}).value.has_value() &&
             harness->runtime->enterRenderPhase().ok &&
             harness->runtime->beginFrame(19).ok,
         "reentrancy fixture enters one shared callback frame");

  ReentrantEventExecutorContext executor{.runtime = harness->runtime.get(),
                                         .callback = callback};
  harness->runtime->setEventExecutor(
      {.context = &executor, .execute = executeReentrantEvent});
  const std::array<LuaScalar, 1> arguments{LuaScalar{0.5}};
  const auto outer = harness->runtime->invoke(callback, arguments);
  expect(executor.nestedFailure &&
             executor.nestedFailure->code == "skin_lua_callback_reentrant" &&
             outer.failure &&
             outer.failure->code == "skin_lua_callback_reentrant",
         "nested callback entry returns the stable reentrancy diagnostic");

  harness->runtime->setEventExecutor({});
  const auto next = harness->runtime->invoke(callback, arguments);
  expect(next.failure &&
             next.failure->code == "skin_lua_event_executor_unavailable",
         "reentrancy rejection leaves the existing frame budget usable");
}

void testCleanTransitionInvalidatesOpenReadHandle() {
  auto harness = makeHarness(LuaRuntimePurpose::Gameplay,
                             "captured_file_operation.luaskin");
  if (!harness) {
    return;
  }
  auto header = harness->runtime->loadHeader();
  if (!header.value) {
    expect(false, "captured-read header succeeds");
    return;
  }
  const LuaCallbackId read = requireCallback(*header.value, "captured_read");
  const LuaCallbackId write = requireCallback(*header.value, "captured_write");
  const LuaCallbackId scan = requireCallback(*header.value, "captured_scan");
  expect(harness->runtime->loadConfigured({}).value.has_value(),
         "captured-read fixture configures");
  expect(harness->runtime->enterRenderPhase().ok,
         "an open read handle is safely invalidated at render transition");
  expect(harness->runtime->beginFrame(1).ok, "captured-read frame begins");
  for (const auto callback : {read, write, scan}) {
    const auto result = harness->runtime->invoke(callback, {});
    expect(result.failure &&
               result.failure->code == "skin_file_render_phase_denied",
           "captured read/write/scan is phase checked after clean transition");
  }
  const auto counters = harness->fileSystem->activityCounters();
  expect(counters.renderReadsDenied == 1 && counters.renderWritesDenied == 1 &&
             counters.renderDirectoryScansDenied == 1,
         "clean transition exercises every exact render denial counter");
  expect(counters.renderReadsPerformed == 0 &&
             counters.renderWritesPerformed == 0 &&
             counters.renderDirectoryScansPerformed == 0,
         "clean transition leaves every matching performed counter at zero");
}

void testDirtyTransitionInvalidatesAllHandlesWithoutOverlayMutation() {
  auto harness =
      makeHarness(LuaRuntimePurpose::Gameplay, "open_handles.luaskin");
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "open-handle fixture header succeeds");
  auto configured = harness->runtime->loadConfigured({});
  if (!configured.value) {
    expect(false, "open-handle fixture configures");
    return;
  }
  const LuaCallbackId read =
      requireCallback(*configured.value, "captured_read");
  const LuaCallbackId write =
      requireCallback(*configured.value, "captured_write");
  const LuaCallbackId scan =
      requireCallback(*configured.value, "captured_scan");
  const fs::path dirtyPath = harness->overlayRoot / "skin/dirty.txt";
  expect(!fs::exists(dirtyPath), "unclosed dirty buffer is not precommitted");

  const auto transition = harness->runtime->enterRenderPhase();
  expect(!transition.ok && transition.failure &&
             transition.failure->code == "skin_file_render_phase_denied",
         "dirty live handle makes transition fail with frozen diagnostic");
  expect(harness->runtime->phase() == LuaRuntimePhase::Configured,
         "failed transition does not publish the render phase");
  expect(!fs::exists(dirtyPath),
         "dirty buffer is discarded and overlay remains unchanged");
  expect(!harness->runtime->beginFrame(1).ok,
         "dirty transition failure keeps frame entry closed");
  expect(!harness->runtime->enterRenderPhase().ok,
         "dirty transition failure cannot be retried into render");
  for (const auto callback : {read, write, scan}) {
    const auto result = harness->runtime->invoke(callback, {});
    expect(result.failure &&
               result.failure->code == "skin_lua_callback_phase_invalid",
           "dirty transition failure keeps callback entry closed");
  }
  const auto counters = harness->fileSystem->activityCounters();
  expect(counters.renderReadsDenied == 0 && counters.renderWritesDenied == 0 &&
             counters.renderDirectoryScansDenied == 0 &&
             counters.renderReadsPerformed == 0 &&
             counters.renderWritesPerformed == 0 &&
             counters.renderDirectoryScansPerformed == 0,
         "closed callback entry performs no filesystem operation");
  expect(!fs::exists(dirtyPath),
         "post-render denials preserve the overlay after dirty discard");
}

} // namespace

int main(int argc, char **argv) {
  testRuntimeContractsUseSourceAuthoritiesAndProvenance();
  testFilesystemReadsTheSelectedEntryWithoutAHostPath();
  testCatalogEntrySourceIsBoundedBeforeHostAllocation();
  testUnrestrictedRuntimeLiftsSourceBudgetWithoutChangingSafeOs();
  testCatalogLuaLoadersBoundSourceBeforeHostAllocation();
  testStrictTwoPhaseStateMachineUsesOneState();
  testMainStateAccessorsOpenOnlyAtRenderTransition();
  testMusicSelectPurposeHasTheConfiguredLiveHost();
  testRuntimeProvidesBeatorajaSafeOsLibrary();
  testRuntimeSearchesVirtualPackagePath();
  testRuntimeCreatesConfiguredDynamicHistoryData();
  testLuaWritesAreVisibleBeforeTheHandleCloses();
  testRequestedExternalLuaSkinHeader();
  testConfiguredTableUsesCanonicalVirtualData();
  testFreshPurposesDoNotShareLuaState();
  testLanguageSurfaceBit32AndTextOnlyLoading();
  testLoadfileUsesBeatorajaRestrictedIoRoot();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "lua_skin_runtime_tests", failures,
      "lua skin runtime test(s) failed", "lua skin runtime tests passed");
}
