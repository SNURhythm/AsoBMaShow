#include "skin/beatoraja/LuaSkinRuntime.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-lua-host-contract-" +
               std::to_string(++serial));
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

struct RuntimeHarness {
  std::unique_ptr<LuaSkinRuntime> runtime;
  LuaSkinFileSystem *fileSystem = nullptr;
};

class HostContractFixture {
public:
  HostContractFixture()
      : roots{.visiblePackages = temp.root() / "visible",
              .privateRevisions =
                  temp.root() / "HOST_ROOT_MUST_NOT_LEAK" / "revisions",
              .privateCatalog = temp.root() / "catalog",
              .profileOverlays = temp.root() / "overlays"},
        package(*normalizePackageId("HostContract").package) {
    const fs::path source = temp.root() / "source";
    writeText(source / "skin/shape.luaskin", R"lua(
if not skin_config then return {type = 0} end

local expected = {
  enabled_options = true,
  file_path = true,
  get_path = true,
  offset = true,
  option = true,
}
local count = 0
for key, _ in pairs(skin_config) do
  assert(expected[key], "unexpected skin_config key: " .. tostring(key))
  count = count + 1
end
assert(count == 5, "skin_config must contain exactly five keys")
for key, _ in pairs(expected) do
  assert(skin_config[key] ~= nil, "missing skin_config key: " .. key)
end

assert(skin_config.option.First == 7)
assert(skin_config.option.Second == 11)
assert(skin_config.option.Third == 7)
assert(#skin_config.enabled_options == 3)
assert(skin_config.enabled_options[1] == 7)
assert(skin_config.enabled_options[2] == 11)
assert(skin_config.enabled_options[3] == 7)
assert(skin_config.file_path.Frame == "red")
assert(skin_config.offset.Authored.x == 1)
assert(skin_config.offset.Authored.y == 2)
assert(skin_config.offset.Authored.w == 3)
assert(skin_config.offset.Authored.h == 4)
assert(skin_config.offset.Authored.r == 5)
assert(skin_config.offset.Authored.a == 6)
return {}
)lua");
    writeText(source / "skin/get_path.luaskin", R"lua(
if not skin_config then return {type = 0} end
local path = skin_config.get_path("parts/frame/*/panel.png")
assert(path == "skin/parts/frame/red/panel.png")
assert(not path:find("HOST_ROOT_MUST_NOT_LEAK", 1, true))
local full_filename = skin_config.get_path("parts/background/*.png")
assert(full_filename == "skin/parts/background/bg.png")
return {}
)lua");
    writeText(source / "skin/get_path_denied.luaskin", R"lua(
if not skin_config then return {type = 0} end
local requests = {
  "parts/random/*.png",
  "parts/frame/*/panel.png",
  "parts/frame/*/still-*.png",
  "/etc/*",
  "../../outside/*",
  "parts/frame/*/panel.png",
  "parts/frame/*",
}
local ok, message = pcall(skin_config.get_path,
                           requests[skin_config.option.Case])
assert(not ok, "unsafe get_path request unexpectedly succeeded")
message = tostring(message)
assert(message:find("@ASOBMSKIN:skin_lua_file_operation_failed:", 1, true) == 1,
       "get_path denial must be a protected host error")
assert(not message:find("HOST_ROOT_MUST_NOT_LEAK", 1, true),
       "get_path denial leaked the host revision root")
return {}
)lua");
    writeText(source / "skin/captured_get_path.luaskin", R"lua(
if not skin_config then return {type = 0} end
local captured_get_path = skin_config.get_path
return {
  after_render = function()
    return captured_get_path("parts/frame/*/panel.png")
  end,
}
)lua");
    writeText(source / "skin/parts/frame/red/panel.png", "selected");
    writeText(source / "skin/parts/frame/blue/panel.png",
              "random-fallback-must-not-win");
    writeText(source / "skin/parts/frame/folder/placeholder.txt",
              "directory-is-not-a-resource");
    writeText(source / "skin/parts/background/bg.png", "selected-filename");
    writeText(source / "skin/parts/random/fallback.png",
              "random-fallback-must-not-win");

    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto snapshot = snapshotter.snapshot(source, package, {}, {});
    expect(snapshot.prepared.has_value(), "host contract fixture snapshots");
    if (snapshot.prepared) {
      prepared.emplace(std::move(*snapshot.prepared));
    }
  }

  std::optional<RuntimeHarness>
  create(std::string_view entryName, LuaRuntimePurpose purpose) {
    if (!prepared) {
      return std::nullopt;
    }
    const auto entry = normalizeEntryPath(
        package, "skin/" + std::string(entryName));
    expect(entry.entry.has_value(), "host contract entry normalizes");
    if (!entry.entry) {
      return std::nullopt;
    }
    auto fileSystem = LuaSkinFileSystem::create(
        {.revision = prepared->readView(),
         .entry = *entry.entry,
         .storageRoots = roots});
    expect(fileSystem.fileSystem != nullptr,
           "host contract filesystem creates");
    if (!fileSystem.fileSystem) {
      return std::nullopt;
    }
    LuaSkinFileSystem *borrowed = fileSystem.fileSystem.get();
    auto runtime = LuaSkinRuntime::create(
        {.purpose = purpose,
         .fileSystem = std::move(fileSystem.fileSystem)});
    expect(runtime.runtime != nullptr, "host contract runtime creates");
    if (!runtime.runtime) {
      return std::nullopt;
    }
    return RuntimeHarness{.runtime = std::move(runtime.runtime),
                          .fileSystem = borrowed};
  }

  const fs::path &hostRoot() const noexcept { return roots.privateRevisions; }

private:
  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageId package;
  AcceptFiles aliases;
  std::optional<PreparedSkinRevision> prepared;
};

HostContractFixture &fixture() {
  static HostContractFixture value;
  return value;
}

BeatorajaSkinConfiguration happyConfiguration() {
  BeatorajaSkinConfiguration configuration;
  configuration.filePaths = {{"Background", "bg.png"}, {"Frame", "red"}};
  configuration.orderedFiles = {
      ConfiguredFile{.name = "Frame",
                     .pattern = "parts/frame/*",
                     .selectedValue = "red"},
      ConfiguredFile{.name = "Background",
                     .pattern = "parts/background/*.png",
                     .selectedValue = "bg.png"}};
  return configuration;
}

void testExactShapeAndEnabledOptionsPreserveAuthoredDuplicates() {
  auto harness = fixture().create("shape.luaskin",
                                  LuaRuntimePurpose::Validation);
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "shape fixture loads its nil-configuration header");

  BeatorajaSkinConfiguration configuration = happyConfiguration();
  configuration.orderedOptions = {{.name = "First", .value = 7},
                                  {.name = "Second", .value = 11},
                                  {.name = "Third", .value = 7}};
  configuration.options = {{"First", 7}, {"Second", 11}, {"Third", 7}};
  configuration.enabledOptionIds = {7, 11};
  configuration.offsets = {
      {"Authored", {.x = 1, .y = 2, .w = 3, .h = 4, .r = 5, .a = 6}}};

  const auto configured = harness->runtime->loadConfigured(configuration);
  expect(configured.value.has_value() && !configured.failure,
         "skin_config exports exactly five keys and enabled_options mirrors "
         "orderedOptions 1:1 without deduplicating selected IDs");
}

void testGetPathSubstitutesAtWildcardAndKeepsTheSuffixVirtual() {
  auto harness = fixture().create("get_path.luaskin",
                                  LuaRuntimePurpose::Validation);
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "get_path fixture loads its header");
  const auto configured =
      harness->runtime->loadConfigured(happyConfiguration());
  expect(configured.value.has_value() && !configured.failure,
         "get_path substitutes the selected value at '*' while preserving "
         "the authored suffix and returns only a normalized package path");
}

BeatorajaSkinConfiguration deniedConfiguration(int caseId) {
  BeatorajaSkinConfiguration configuration = happyConfiguration();
  configuration.orderedOptions = {{.name = "Case", .value = caseId}};
  configuration.options = {{"Case", caseId}};
  configuration.enabledOptionIds = {caseId};
  switch (caseId) {
  case 2:
    configuration.filePaths = {{"Frame A", "red"}, {"Frame B", "blue"}};
    configuration.orderedFiles = {
        {.name = "Frame A",
         .pattern = "parts/frame/*",
         .selectedValue = "red"},
        {.name = "Frame B",
         .pattern = "parts/frame/*",
         .selectedValue = "blue"}};
    break;
  case 4:
    configuration.filePaths = {{"Host", "passwd"}};
    configuration.orderedFiles = {
        {.name = "Host", .pattern = "/etc/*", .selectedValue = "passwd"}};
    break;
  case 5:
    configuration.filePaths = {{"Escape", "file.png"}};
    configuration.orderedFiles = {
        {.name = "Escape",
         .pattern = "../../outside/*",
         .selectedValue = "file.png"}};
    break;
  case 6:
    configuration.filePaths = {{"Frame", "missing"}};
    configuration.orderedFiles.front().selectedValue = "missing";
    break;
  case 7:
    configuration.filePaths = {{"Frame", "folder"}};
    configuration.orderedFiles.front().selectedValue = "folder";
    break;
  default:
    break;
  }
  return configuration;
}

void testGetPathRejectsUnsafeAndNondeterministicResolution() {
  static constexpr std::string_view cases[] = {
      "unconfigured pattern",       "ambiguous configured patterns",
      "unresolved wildcard",        "absolute host path",
      "package escape",             "missing selected resource",
      "non-regular selected resource",
  };
  for (int caseId = 1; caseId <= 7; ++caseId) {
    auto harness = fixture().create("get_path_denied.luaskin",
                                    LuaRuntimePurpose::Validation);
    if (!harness) {
      continue;
    }
    expect(harness->runtime->loadHeader().value.has_value(),
           "get_path denial fixture loads its header");
    const auto configured =
        harness->runtime->loadConfigured(deniedConfiguration(caseId));
    expect(configured.value.has_value() && !configured.failure,
           cases[caseId - 1]);
  }
}

void testCapturedGetPathLosesAuthorityAtRenderTransition() {
  auto harness = fixture().create("captured_get_path.luaskin",
                                  LuaRuntimePurpose::Gameplay);
  if (!harness) {
    return;
  }
  expect(harness->runtime->loadHeader().value.has_value(),
         "captured get_path fixture loads its header");
  auto configured =
      harness->runtime->loadConfigured(happyConfiguration());
  expect(configured.value.has_value(), "captured get_path fixture configures");
  if (!configured.value) {
    return;
  }
  const auto callback = configured.value->callbackNamed("after_render");
  expect(callback.has_value(), "configured table retains captured get_path");
  if (!callback) {
    return;
  }
  expect(harness->runtime->enterRenderPhase().ok &&
             harness->runtime->beginFrame(1).ok,
         "captured get_path fixture reaches render");
  const auto denied = harness->runtime->invoke(*callback, {});
  expect(denied.failure &&
             denied.failure->code == "skin_file_render_phase_denied",
         "captured get_path closure is denied after render transition");
  if (denied.failure) {
    expect(denied.failure->message.find(fixture().hostRoot().string()) ==
                   std::string::npos &&
               denied.failure->virtualPath.find(
                   fixture().hostRoot().string()) == std::string::npos,
           "post-render get_path denial does not leak the host root");
  }
  const auto counters = harness->fileSystem->activityCounters();
  expect(counters.renderReadsDenied == 1 &&
             counters.renderReadsPerformed == 0,
         "captured get_path records one denial and performs no render read");
}

} // namespace

int main() {
  testExactShapeAndEnabledOptionsPreserveAuthoredDuplicates();
  testGetPathSubstitutesAtWildcardAndKeepsTheSuffixVirtual();
  testGetPathRejectsUnsafeAndNondeterministicResolution();
  testCapturedGetPathLosesAuthorityAtRenderTransition();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "lua skin host modules tests passed\n";
  return 0;
}
