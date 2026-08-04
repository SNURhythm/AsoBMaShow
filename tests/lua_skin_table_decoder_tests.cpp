#include "skin/beatoraja/LuaSkinTableDecoder.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
              ("asobmashow-lua-header-test-" + std::to_string(++serial));
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

class HeaderFixture {
public:
  HeaderFixture()
      : roots{.visiblePackages = temp.root() / "visible",
              .privateRevisions = temp.root() / "revisions",
              .privateCatalog = temp.root() / "catalog",
              .profileOverlays = temp.root() / "overlays"},
        package(*normalizePackageId("HeaderContract").package) {
    const fs::path source = temp.root() / "source";
    writeText(source / "skin/valid.luaskin", R"lua(
local properties = {}
properties[2] = {
  category = "Play", name = "Gauge",
  item = {{name = "Normal", op = 11}}, def = "Normal"
}
properties[1] = {
  category = "Play", name = "Lane type",
  item = {{name = "Default", op = 927}, {name = "Wide", op = 928}},
  def = "Default"
}
return {
  type = "0", w = "1920", h = 1080, name = 42, author = "fixture",
  category = {{name = "Play", item = {"Lane type", "Gauge"}}},
  property = properties,
  filepath = {{category = "Play", name = "Background",
               path = "images/*.png", def = "bg.png"}},
  offset = {{category = "Play", name = "Authored offset", id = 120,
             x = 0, y = false, w = true, h = false, r = true, a = false}},
  unknown = {ignored = true}
}
)lua");
    writeText(source / "skin/hole.luaskin", R"lua(
return {type=0, property={[2]={name="B",item={{name="B",op=2}},def="B"}}}
)lua");
    writeText(source / "skin/mixed.luaskin", R"lua(
return {type=0, property={[1]={name="A",item={{name="A",op=1}},def="A"},
                         extra={name="B",item={{name="B",op=2}},def="B"}}}
)lua");
    writeText(source / "skin/numeric-string.luaskin", R"lua(
return {type=0, property={["1"]={name="A",item={{name="A",op=1}},def="A"}}}
)lua");
    writeText(source / "skin/aliases.luaskin", R"lua(
return {type=0, width=999, height=888, options={{name="ignored"}}}
)lua");
    writeText(source / "skin/other-type.luaskin", R"lua(
return {type=5, name="catalog header", offset={{name="Authored",id=100,x=true}}}
)lua");
    writeText(source / "skin/fractional.luaskin", R"lua(
return {type=0.9, w=17.9, h="720.8"}
)lua");
    writeText(source / "skin/nonnumeric.luaskin", R"lua(
return {type="not-a-number"}
)lua");
    writeText(source / "skin/unknown-type.luaskin", R"lua(
return {type=19}
)lua");
    writeText(source / "skin/non-table-arrays.luaskin", R"lua(
return {type=0, category=42, property="none", filepath=false,
        offset=function() end}
)lua");
    writeText(source / "skin/duplicates.luaskin", R"lua(
return {type=0, property={
  {name="A",item={{name="One",op=1}},def="One"},
  {name="A",item={{name="Two",op=2}},def="Two"}}}
)lua");
    writeText(source / "skin/id-collision.luaskin", R"lua(
return {type=0, property={
  {name="A",item={{name="One",op=7}},def="One"},
  {name="B",item={{name="Two",op=7}},def="Two"}}}
)lua");
    writeText(source / "skin/synth-collision.luaskin", R"lua(
return {type=0, offset={{name="Custom",id=30,h=true}}}
)lua");
    writeText(source / "skin/too-many.luaskin", R"lua(
local p = {}
for i=1,257 do p[i]={name="P"..i,item={{name="V",op=1000+i}},def="V"} end
return {type=0, property=p}
)lua");
    writeText(source / "skin/images/bg.png", "fixture");

    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto snapshot = snapshotter.snapshot(source, package, {}, {});
    expect(snapshot.prepared.has_value(), "header fixture snapshots");
    if (snapshot.prepared) {
      prepared.emplace(std::move(*snapshot.prepared));
    }
  }

  HeaderDecodeResult decode(std::string_view filename) {
    if (!prepared) {
      return {};
    }
    const auto entry =
        *normalizeEntryPath(package, "skin/" + std::string(filename)).entry;
    auto fileSystem =
        LuaSkinFileSystem::create({.revision = prepared->readView(),
                                   .entry = entry,
                                   .storageRoots = roots});
    expect(fileSystem.fileSystem != nullptr, "decoder filesystem creates");
    if (!fileSystem.fileSystem) {
      return {};
    }
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Catalog,
         .fileSystem = std::move(fileSystem.fileSystem)});
    expect(created.runtime != nullptr, "decoder runtime creates");
    if (!created.runtime) {
      return {};
    }
    auto value = created.runtime->loadHeader();
    expect(value.value.has_value(), "decoder fixture returns a Lua value");
    if (!value.value) {
      return {};
    }
    return LuaSkinTableDecoder{}.decodeHeader(*value.value);
  }

  std::unique_ptr<LuaSkinFileSystem>
  fileSystem(std::string_view filename = "valid.luaskin") {
    if (!prepared) {
      return {};
    }
    const auto entry =
        *normalizeEntryPath(package, "skin/" + std::string(filename)).entry;
    return LuaSkinFileSystem::create({.revision = prepared->readView(),
                                      .entry = entry,
                                      .storageRoots = roots})
        .fileSystem;
  }

private:
  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageId package;
  AcceptFiles aliases;
  std::optional<PreparedSkinRevision> prepared;
};

HeaderFixture &fixture() {
  static HeaderFixture value;
  return value;
}

void testTypedHeaderPreservesAuthoredNumericOrderAndCoercions() {
  const auto result = fixture().decode("valid.luaskin");
  expect(result.header.has_value() && result.diagnostics.empty(),
         "valid typed header decodes without diagnostics");
  if (!result.header) {
    return;
  }
  const auto &header = *result.header;
  expect(header.type == 0 && header.width == 1920 && header.height == 1080,
         "root numeric fields use traced integer coercions");
  expect(header.name == "42" && header.author == "fixture",
         "root text fields use traced string coercions");
  expect(header.options.size() == 2 && header.options[0].name == "Lane type" &&
             header.options[1].name == "Gauge",
         "authored arrays preserve numeric 1..n order, not insertion order");
  expect(header.options[0].choices.size() == 2 &&
             header.options[0].choices[1].value == 928,
         "option choices decode into typed records");
  expect(header.offsets.size() == 5 &&
             header.offsets[0].name == "Authored offset" &&
             header.offsets[0].permissions ==
                 (kOffsetPermissionX | kOffsetPermissionW | kOffsetPermissionR),
         "authored offset and permission mask are preserved");
  expect(
      header.offsets[1].name == "All offset(%)" && header.offsets[1].id == 10 &&
          header.offsets[1].permissions == 0x0f &&
          header.offsets[2].name == "Notes offset" &&
          header.offsets[2].id == 30 &&
          header.offsets[2].permissions == kOffsetPermissionH &&
          header.offsets[3].id == 32 && header.offsets[3].permissions == 0x2f &&
          header.offsets[4].id == 33 && header.offsets[4].permissions == 0x2f,
      "7-key headers append the four exact Beatoraja offsets");
}

void testStrictArraysAndHeaderBoundsFailClosed() {
  for (const std::string_view invalid :
       {"hole.luaskin", "mixed.luaskin", "numeric-string.luaskin",
        "too-many.luaskin"}) {
    const auto result = fixture().decode(invalid);
    expect(!result.header && !result.diagnostics.empty(),
           "invalid authored array or limit is rejected");
  }
  const auto aliases = fixture().decode("aliases.luaskin");
  expect(aliases.header && aliases.header->width == 1280 &&
             aliases.header->height == 720 && aliases.header->options.empty(),
         "unknown aliases are ignored and canonical field defaults remain");
  const auto otherType = fixture().decode("other-type.luaskin");
  expect(otherType.header && otherType.header->type == 5 &&
             otherType.header->offsets.size() == 1 &&
             otherType.header->offsets.front().name == "Authored",
         "catalog headers preserve non-7K type without synthesized offsets");
  const auto fractional = fixture().decode("fractional.luaskin");
  expect(fractional.header && fractional.header->type == 0 &&
             fractional.header->width == 17 && fractional.header->height == 720,
         "integer conversion truncates numeric values like pinned LuaValue");
  const auto nonnumeric = fixture().decode("nonnumeric.luaskin");
  expect(!nonnumeric.header && !nonnumeric.diagnostics.empty(),
         "nonnumeric strings do not silently coerce to zero");
  const auto unknownType = fixture().decode("unknown-type.luaskin");
  expect(!unknownType.header && !unknownType.diagnostics.empty(),
         "unknown Beatoraja skin type IDs are rejected");
  const auto nonTables = fixture().decode("non-table-arrays.luaskin");
  expect(nonTables.header && nonTables.header->categories.empty() &&
             nonTables.header->options.empty() &&
             nonTables.header->files.empty() &&
             nonTables.header->offsets.size() == 4,
         "present non-table authored vectors convert to empty arrays");
}

void testSemanticAndSynthesizedCollisionsFailClosed() {
  for (const std::string_view invalid :
       {"duplicates.luaskin", "id-collision.luaskin",
        "synth-collision.luaskin"}) {
    const auto result = fixture().decode(invalid);
    expect(!result.header && !result.diagnostics.empty(),
           "ambiguous configuration name or ID is rejected");
  }
}

void testReconciliationDefaultsSanitizesAndIndexesConfiguration() {
  const auto decoded = fixture().decode("valid.luaskin");
  auto fileSystem = fixture().fileSystem();
  expect(decoded.header && fileSystem, "reconciliation fixture is available");
  if (!decoded.header || !fileSystem) {
    return;
  }
  EntryProfileSettings saved;
  saved.options = {{"Lane type", 928}, {"Gauge", 999}, {"Removed", 5}};
  saved.filePaths = {{"Background", "missing.png"}, {"Removed", "x.png"}};
  saved.offsets = {
      {"Authored offset", {.x = 1, .y = 2, .w = 3, .h = 4, .r = 5, .a = 6}},
      {"Notes offset", {.x = 7, .y = 8, .w = 9, .h = 10, .r = 11, .a = 12}},
      {"Removed", {.x = 99}}};
  saved.viewport.mode = ViewportMode::Stretch;

  const auto reconciled =
      reconcileSkinConfiguration(*decoded.header, &saved, *fileSystem);
  expect(reconciled.configuration && reconciled.diagnostics.empty(),
         "saved settings reconcile into a canonical configuration");
  if (!reconciled.configuration) {
    return;
  }
  const auto &configuration = *reconciled.configuration;
  expect(
      reconciled.reconciledSettings.options ==
              std::map<std::string, int>{{"Gauge", 11}, {"Lane type", 928}} &&
          configuration.orderedOptions.size() == 2 &&
          configuration.orderedOptions[0].value == 928 &&
          configuration.enabledOptionIds == std::set<int>{11, 928},
      "declared saved option survives and invalid/removed values reset");
  expect(reconciled.reconciledSettings.filePaths ==
                 std::map<std::string, std::string>{{"Background", "bg.png"}} &&
             configuration.filePaths.at("Background") == "bg.png",
         "invalid file choice resets to the deterministic declared default");
  expect(reconciled.reconciledSettings.viewport.mode == ViewportMode::Stretch,
         "viewport remains profile-owned and outside configuration digest");
  const auto authored = configuration.offsets.at("Authored offset");
  const auto notes = configuration.offsets.at("Notes offset");
  expect(authored == ConfigOffset{.x = 1, .w = 3, .r = 5} &&
             notes == ConfigOffset{.h = 10},
         "disallowed offset components are zeroed before persistence/export");
  expect(configuration.offsetsById.at(120) == authored &&
             configuration.offsetsById.at(30) == notes,
         "sanitized offsets are indexed by unambiguous declared IDs");
}

void testConfigurationDigestUsesTheFrozenBigEndianGrammar() {
  BeatorajaSkinConfiguration configuration;
  configuration.options = {{"A", -1}};
  configuration.filePaths = {{"F", "x.png"}};
  configuration.offsets = {
      {"O", {.x = 1, .y = -2, .w = 3, .h = -4, .r = 5, .a = -6}}};
  expect(skinConfigurationDigest(configuration) ==
             "70c9d314aceda1a42e87492ff250c9765aaaee58e1583a3fe79badf83d1f1515",
         "configuration digest matches the literal signed big-endian vector");
  expect(skinConfigurationDigest({}) ==
             "f3c2c52f1de34a366df4f5bad4eb6a5bc080153949ea6422cb81aebfc84bc4b3",
         "empty configuration digest matches the frozen vector");
}

} // namespace

int main() {
  testTypedHeaderPreservesAuthoredNumericOrderAndCoercions();
  testStrictArraysAndHeaderBoundsFailClosed();
  testSemanticAndSynthesizedCollisionsFailClosed();
  testReconciliationDefaultsSanitizesAndIndexesConfiguration();
  testConfigurationDigestUsesTheFrozenBigEndianGrammar();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "lua skin table decoder tests passed\n";
  return 0;
}
