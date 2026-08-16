#include "skin/beatoraja/LuaSkinTableDecoder.h"

#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

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

void copyLuaSources(const fs::path &source, const fs::path &destination) {
  for (const fs::directory_entry &entry :
       fs::recursive_directory_iterator(source)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string extension = entry.path().extension().string();
    if (extension != ".lua" && extension != ".luaskin") {
      continue;
    }
    const fs::path relative = fs::relative(entry.path(), source);
    fs::create_directories((destination / relative).parent_path());
    fs::copy_file(entry.path(), destination / relative,
                  fs::copy_options::overwrite_existing);
  }
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
    writeText(source / "skin/numeric-glyphs.luaskin", R"lua(
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "atlas", path = "atlas.png"}},
  value = {
    {id = "signed-number", src = "atlas", x = 0, y = 0,
     w = 480, h = 10, divx = 48, divy = 1, timer = 7, cycle = 240,
     digit = 12, align = 2, padding = 1, zeropadding = 2, space = 3,
     ref = 101,
     offset = {{x = 1.5, y = -2.5, w = 3.5, h = 4.5},
               {x = 5.5, y = 6.5, w = -7.5, h = 8.5}}},
    {id = "plain-number", src = "atlas", x = 0, y = 20,
     w = 200, h = 10, divx = 20, divy = 1, cycle = 120,
     digit = 4, align = 1, padding = 1, zeropadding = 2, space = 2,
     ref = 102},
  },
  floatvalue = {
    {id = "signed-float", src = "atlas", x = 0, y = 40,
     w = 440, h = 10, divx = 44, divy = 1, timer = 8, cycle = 360,
     iketa = 7, fketa = 5, align = 0, zeropadding = 1, space = 4,
     isSignvisible = true, gain = 1.25, ref = 201,
     offset = {{x = 9.5, y = 10.5, w = 11.5, h = 12.5}}},
  },
  destination = {
    {id = "signed-number", dst = {{time = 0}}},
    {id = "plain-number", dst = {{time = 0}}},
    {id = "signed-float", dst = {{time = 0}}},
  },
}
)lua");
    writeText(source / "skin/numeric-offset-limit.luaskin", R"lua(
local offsets = {}
for i = 1, 257 do
  offsets[i] = {x = i, y = -i, w = i + 0.5, h = i + 1.5}
end
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "atlas", path = "atlas.png"}},
  value = {{id = "number", src = "atlas", w = 100, h = 10,
            divx = 10, divy = 1, digit = 4, ref = 1,
            offset = offsets}},
  destination = {{id = "number", dst = {{}}}},
}
)lua");
    writeText(source / "skin/numeric-offset-nonfinite.luaskin", R"lua(
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "atlas", path = "atlas.png"}},
  floatvalue = {{id = "number", src = "atlas", w = 120, h = 10,
                 divx = 12, divy = 1, iketa = 2, fketa = 2, ref = 1,
                 offset = {{x = 0 / 0, y = 2, w = 3, h = 4}}}},
  destination = {{id = "number", dst = {{}}}},
}
)lua");
    writeText(source / "skin/numeric-budget-boundary.luaskin", R"lua(
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "atlas", path = "atlas.png"}},
  image = {{id = "filler", src = "atlas", w = 199976, h = 1,
            divx = 199976, divy = 1}},
  floatvalue = {{id = "number", src = "atlas", w = 22, h = 1,
                 divx = 22, divy = 1, iketa = 2, fketa = 2, ref = 1}},
  destination = {{id = "filler", dst = {{}}},
                 {id = "number", dst = {{}}}},
}
)lua");
    writeText(source / "skin/numeric-budget-exceeded.luaskin", R"lua(
return {
  type = 0, w = 1280, h = 720,
  source = {{id = "atlas", path = "atlas.png"}},
  image = {{id = "filler", src = "atlas", w = 199978, h = 1,
            divx = 199978, divy = 1}},
  floatvalue = {{id = "number", src = "atlas", w = 22, h = 1,
                 divx = 22, divy = 1, iketa = 2, fketa = 2, ref = 1}},
  destination = {{id = "filler", dst = {{}}},
                 {id = "number", dst = {{}}}},
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
    writeText(source / "skin/missing-option-item.luaskin", R"lua(
return {type=0, property={{name="No choices", def="Default"}}}
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
    writeText(source / "skin/dimension-zero.luaskin", R"lua(
return {type=5, w=0, h=720}
)lua");
    writeText(source / "skin/dimension-negative.luaskin", R"lua(
return {type=5, w=1280, h=-1}
)lua");
    writeText(source / "skin/dimension-too-wide.luaskin", R"lua(
return {type=5, w=8193, h=720}
)lua");
    writeText(source / "skin/dimension-too-tall.luaskin", R"lua(
return {type=5, w=1280, h=8193}
)lua");
    writeText(source / "skin/dimension-boundaries.luaskin", R"lua(
return {type=5, w=1, h=8192}
)lua");
    writeText(source / "skin/missing-category-item.luaskin", R"lua(
return {type=5, category={{name="Category", item={[1]="A", [3]="B"}}}}
)lua");
    writeText(source / "skin/empty-category-item.luaskin", R"lua(
return {type=5, category={{name="Category", item={""}}}}
)lua");
    writeText(source / "skin/missing-option-name.luaskin", R"lua(
return {type=5, property={{item={{name="Value", op=1}}}}}
)lua");
    writeText(source / "skin/empty-option-name.luaskin", R"lua(
return {type=5, property={{name="", item={{name="Value", op=1}}}}}
)lua");
    writeText(source / "skin/missing-choice-label.luaskin", R"lua(
return {type=5, property={{name="Option", item={{op=1}}}}}
)lua");
    writeText(source / "skin/empty-choice-label.luaskin", R"lua(
return {type=5, property={{name="Option", item={{name="", op=1}}}}}
)lua");
    writeText(source / "skin/missing-file-name.luaskin", R"lua(
return {type=5, filepath={{path="ordinary/*.png"}}}
)lua");
    writeText(source / "skin/empty-file-name.luaskin", R"lua(
return {type=5, filepath={{name="", path="ordinary/*.png"}}}
)lua");
    writeText(source / "skin/missing-file-path.luaskin", R"lua(
return {type=5, filepath={{name="File"}}}
)lua");
    writeText(source / "skin/empty-file-path.luaskin", R"lua(
return {type=5, filepath={{name="File", path=""}}}
)lua");
    writeText(source / "skin/missing-offset-name.luaskin", R"lua(
return {type=5, offset={{id=100, x=true}}}
)lua");
    writeText(source / "skin/empty-offset-name.luaskin", R"lua(
return {type=5, offset={{name="", id=100, x=true}}}
)lua");
    writeText(source / "skin/optional-text.luaskin", R"lua(
return {
  type=5,
  property={{name="Option", item={{name="Value", op=1}}}},
  filepath={{name="File", path="ordinary/*.png"}},
  offset={{name="Offset", id=100}}
}
)lua");
    writeText(source / "skin/patterns.luaskin", R"lua(
return {type=5, filepath={
  {name="Ordinary", path="ordinary/*.png"},
  {name="Uppercase", path="uppercase/*.png"},
  {name="Player 1", path="characters/*|1P|"},
  {name="Player suffix", path="portraits/*|1P|.png"}
}}
)lua");
    writeText(source / "skin/crowded.luaskin", R"lua(
return {type=5, filepath={{name="Crowded", path="crowded/*.png"}}}
)lua");
    writeText(source / "skin/duplicates.luaskin", R"lua(
return {type=0, property={
  {name="A",item={{name="One",op=1}},def="One"},
  {name="A",item={{name="Two",op=2}},def="Two"}}}
)lua");
    writeText(source / "skin/duplicate-file-name.luaskin", R"lua(
return {type=5, filepath={
  {name="Reused", path="ordinary/*.png"},
  {name="Reused", path="uppercase/*.png"}
}}
)lua");
    writeText(source / "skin/invalid-file-pattern.luaskin", R"lua(
return {type=5, filepath={{name="Broken", path="ordinary/no-wildcard.png"}}}
)lua");
    writeText(source / "skin/empty-choices.luaskin", R"lua(
return {type=5, property={{name="Unconfigured", item={}}}}
)lua");
    writeText(source / "skin/unresolved-file.luaskin", R"lua(
return {type=5, filepath={{name="Not installed", path="missing/*.png"}}}
)lua");
    writeText(source / "skin/long-author.luaskin", R"lua(
return {type=5, author=string.rep("A", 2048)}
)lua");
    writeText(source / "skin/long-configuration-text.luaskin", R"lua(
local text = string.rep("X", 136)
return {
  type=5,
  category={{name=text, item={1}}},
  property={{category=text, name=text, def=text, item={{name=text, op=901}}}},
  filepath={{category=text, name=text, path="ordinary/*.png", def="default"}},
  offset={{category=text, name=text, id=120, x=true}},
}
)lua");
    writeText(source / "skin/unbounded-configuration-text.luaskin", R"lua(
local text = string.rep("X", 8 * 1024 * 1024 + 1)
return {
  type=5,
  property={{name=text, item={{name="Value", op=901}}}},
}
)lua");
    writeText(source / "skin/duplicate-font.luaskin", R"lua(
return {
  type=0,
  font={{id="shared", path="first.ttf"}, {id="shared", path="second.ttf"}},
  text={{id="caption", font="shared", size=24, ref=10}},
  destination={{id="caption", dst={{}}}}
}
)lua");
    writeText(source / "skin/external-resource-paths.luaskin", R"lua(
return {
  type=0,
  source={{id="atlas", path="../shared/atlas.png"}},
  font={{id="shared", path="../shared/font.ttf",
         fallback={{path="/Library/Fonts/Arial.ttf"}}}},
  text={{id="caption", font="shared", size=24, ref=10}},
  destination={{id="caption", dst={{}}}}
}
)lua");
    writeText(source / "skin/system/relative-file-pattern.luaskin", R"lua(
return {type=5, filepath={{name="Settings", path="../customize/settings/7keys/*",
                            def="default.lua"}}}
)lua");
    writeText(source / "skin/system/escaping-file-pattern.luaskin", R"lua(
return {type=5, filepath={{name="Outside", path="../../../outside/*"}}}
)lua");
    writeText(source / "skin/duplicate-offset-id.luaskin", R"lua(
return {type=5, offset={
  {name="First", id=120, x=true},
  {name="Second", id=120, y=true}
}}
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
    writeText(source / "skin/ordinary/plain.png", "fixture");
    writeText(source / "skin/ordinary/ignored.txt", "fixture");
    writeText(source / "skin/uppercase/Cover.PNG", "fixture");
    writeText(source / "skin/characters/Alpha.webp", "fixture");
    writeText(source / "skin/characters/Zulu.png", "fixture");
    writeText(source / "skin/portraits/Portrait.PNG", "fixture");
    writeText(source / "skin/portraits/ignored.txt", "fixture");
    writeText(source / "skin/customize/settings/7keys/default.lua", "fixture");
    for (int index = 0; index < 300; ++index) {
      writeText(source / "skin/crowded" /
                    ("unrelated-" + std::to_string(index) + ".txt"),
                "fixture");
    }
    writeText(source / "skin/crowded/match.png", "fixture");

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

  BeatorajaSkinModelDecodeResult decodeGameplay(std::string_view filename) {
    auto runtimeFileSystem = fileSystem(filename);
    auto reconciliationFileSystem = fileSystem(filename);
    expect(runtimeFileSystem != nullptr && reconciliationFileSystem != nullptr,
           "gameplay decoder filesystems create");
    if (!runtimeFileSystem || !reconciliationFileSystem) {
      return {};
    }
    auto created =
        LuaSkinRuntime::create({.purpose = LuaRuntimePurpose::Validation,
                                .fileSystem = std::move(runtimeFileSystem)});
    expect(created.runtime != nullptr, "gameplay decoder runtime creates");
    if (!created.runtime) {
      return {};
    }
    auto headerValue = created.runtime->loadHeader();
    expect(headerValue.value.has_value(), "gameplay header executes");
    if (!headerValue.value) {
      return {};
    }
    LuaSkinTableDecoder decoder;
    const auto header = decoder.decodeHeader(*headerValue.value);
    expect(header.header.has_value(), "gameplay header decodes");
    if (!header.header) {
      return {};
    }
    headerValue.value.reset();
    const auto reconciled = reconcileSkinConfiguration(
        *header.header, nullptr, *reconciliationFileSystem);
    expect(reconciled.configuration.has_value(),
           "gameplay configuration reconciles");
    if (!reconciled.configuration) {
      return {};
    }
    auto configured =
        created.runtime->loadConfigured(*reconciled.configuration);
    expect(configured.value.has_value(), "gameplay configured phase executes");
    if (!configured.value) {
      return {};
    }
    return decoder.decodeGameplay(*configured.value,
                                  {.runtime = *created.runtime});
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

const SkinObjectDefinition *objectNamed(const BeatorajaSkinModel &model,
                                        std::string_view name);

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

void testHeaderArraysFollowBeatorajaTableKeys() {
  const auto hole = fixture().decode("hole.luaskin");
  expect(hole.header && hole.header->options.size() == 1 &&
             hole.header->options.front().name == "B" &&
             hole.header->options.front().choices.front().value == 2,
         "sparse authored option tables retain their present values");
  const auto mixed = fixture().decode("mixed.luaskin");
  expect(mixed.header && mixed.header->options.size() == 2 &&
             std::ranges::any_of(mixed.header->options,
                                 [](const auto &option) {
                                   return option.name == "A" &&
                                          option.choices.size() == 1 &&
                                          option.choices.front().value == 1;
                                 }) &&
             std::ranges::any_of(mixed.header->options,
                                 [](const auto &option) {
                                   return option.name == "B" &&
                                          option.choices.size() == 1 &&
                                          option.choices.front().value == 2;
                                 }),
         "mixed-key authored option tables retain every table value");
  const auto numericString = fixture().decode("numeric-string.luaskin");
  expect(numericString.header && numericString.header->options.size() == 1 &&
             numericString.header->options.front().name == "A",
         "string-keyed authored option tables retain their present values");
  const auto missingItem = fixture().decode("missing-option-item.luaskin");
  expect(missingItem.header && missingItem.header->options.size() == 1 &&
             missingItem.header->options.front().choices.empty(),
         "missing option items decode as Beatoraja's empty array");
  const auto tooMany = fixture().decode("too-many.luaskin");
  expect(tooMany.header && tooMany.header->options.size() == 257,
         "Beatoraja headers preserve every authored custom option without an "
         "app-defined count limit");
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

void testAuthoredDimensionsStayWithinTheDecoderBoundary() {
  for (const std::string_view invalid :
       {"dimension-zero.luaskin", "dimension-negative.luaskin"}) {
    const auto result = fixture().decode(invalid);
    expect(!result.header && !result.diagnostics.empty(),
           "non-positive authored dimensions are rejected");
  }
  const auto tooWide = fixture().decode("dimension-too-wide.luaskin");
  const auto tooTall = fixture().decode("dimension-too-tall.luaskin");
  expect(tooWide.header && tooWide.header->width == 8193 && tooTall.header &&
             tooTall.header->height == 8193,
         "catalog headers preserve authored dimensions without an app-defined "
         "maximum");
  const auto boundaries = fixture().decode("dimension-boundaries.luaskin");
  expect(boundaries.header && boundaries.header->width == 1 &&
             boundaries.header->height ==
                 LuaSkinTableDecoderPolicy::maxGameplayDimension,
         "ordinary authored dimensions remain valid");
}

void testHeaderTextFollowsLuaValueCoercion() {
  for (const std::string_view fixtureName :
       {"missing-category-item.luaskin", "empty-category-item.luaskin",
        "missing-option-name.luaskin", "empty-option-name.luaskin",
        "missing-choice-label.luaskin", "empty-choice-label.luaskin",
        "missing-file-name.luaskin", "empty-file-name.luaskin",
        "missing-file-path.luaskin", "empty-file-path.luaskin",
        "missing-offset-name.luaskin", "empty-offset-name.luaskin"}) {
    const auto result = fixture().decode(fixtureName);
    expect(result.header && result.diagnostics.empty(),
           "header text follows Beatoraja's permissive LuaValue coercion");
  }

  const auto optional = fixture().decode("optional-text.luaskin");
  expect(optional.header && optional.header->name.empty() &&
             optional.header->author.empty() &&
             optional.header->options.front().category.empty() &&
             optional.header->options.front().defaultLabel.empty() &&
             optional.header->files.front().category.empty() &&
             optional.header->files.front().defaultValue.empty(),
         "root metadata, categories, and authored defaults remain optional");
}

void testHeaderPreservesBeatorajaConfigurationDeclarations() {
  for (const std::string_view fixtureName :
       {"duplicates.luaskin", "id-collision.luaskin", "synth-collision.luaskin",
        "duplicate-file-name.luaskin", "invalid-file-pattern.luaskin",
        "empty-choices.luaskin"}) {
    const auto result = fixture().decode(fixtureName);
    expect(result.header && result.diagnostics.empty(),
           std::string("Beatoraja header preserves ") +
               std::string(fixtureName));
  }
}

void testHeaderDoesNotImposeAnUnpinnedTextLimit() {
  const auto decoded = fixture().decode("long-author.luaskin");
  expect(decoded.header && decoded.diagnostics.empty() &&
             decoded.header->author.size() == 2048,
         "a valid Beatoraja header is not rejected by an invented 1 KiB "
         "metadata limit");
}

void testHeaderAndConfigurationAcceptLongBeatorajaNames() {
  const auto decoded = fixture().decode("long-configuration-text.luaskin");
  expect(decoded.header && decoded.diagnostics.empty() &&
             decoded.header->categories.size() == 1 &&
             decoded.header->categories.front().name.size() == 136 &&
             decoded.header->options.size() == 1 &&
             decoded.header->options.front().name.size() == 136 &&
             decoded.header->options.front().defaultLabel.size() == 136 &&
             decoded.header->files.size() == 1 &&
             decoded.header->files.front().name.size() == 136 &&
             decoded.header->offsets.size() == 1 &&
             decoded.header->offsets.front().name.size() == 136,
         "Beatoraja headers do not inherit the app profile's old 128-byte "
         "configuration-name cap");
  if (!decoded.header) {
    return;
  }

  auto fileSystem = fixture().fileSystem("long-configuration-text.luaskin");
  expect(fileSystem != nullptr,
         "long Beatoraja configuration names have a reconciliation filesystem");
  if (!fileSystem) {
    return;
  }
  const auto reconciled =
      reconcileSkinConfiguration(*decoded.header, nullptr, *fileSystem);
  const std::string key(136, 'X');
  expect(reconciled.configuration && reconciled.diagnostics.empty() &&
             reconciled.reconciledSettings.options ==
                 std::map<std::string, int>{{key, 901}} &&
             reconciled.reconciledSettings.filePaths ==
                 std::map<std::string, std::string>{{key, "plain.png"}} &&
             reconciled.reconciledSettings.offsets.contains(key),
         "long authored names are retained through the Beatoraja configuration "
         "reconciliation path");

  const auto package = normalizePackageId("LongConfiguration").package;
  const auto entry =
      package ? normalizeEntryPath(*package, "skin/main.luaskin").entry
              : std::nullopt;
  SkinProfileSettings profile;
  if (entry) {
    profile.entries.emplace(*entry, reconciled.reconciledSettings);
  }
  profile.sanitize();
  expect(entry && profile.entries.contains(*entry) &&
             profile.entries.at(*entry).options.contains(key) &&
             profile.entries.at(*entry).filePaths.contains(key) &&
             profile.entries.at(*entry).offsets.contains(key),
         "profile sanitization retains valid long Beatoraja configuration "
         "names instead of discarding their selections");
}

void testHeaderAndProfileConfigurationDoNotCapAuthoredText() {
  const auto decoded = fixture().decode("unbounded-configuration-text.luaskin");
  constexpr std::size_t authoredTextBytes = 8 * 1024 * 1024 + 1;
  expect(decoded.header && decoded.diagnostics.empty() &&
             decoded.header->options.size() == 1 &&
             decoded.header->options.front().name.size() == authoredTextBytes,
         "Beatoraja header configuration text is not capped at an app-defined "
         "copy budget");
  if (!decoded.header) {
    return;
  }

  EntryProfileSettings settings;
  settings.options.emplace(decoded.header->options.front().name, 901);
  SkinProfileSettings profile;
  const auto package = normalizePackageId("UnboundedConfiguration").package;
  const auto entry =
      package ? normalizeEntryPath(*package, "skin/main.luaskin").entry
              : std::nullopt;
  if (entry) {
    profile.entries.emplace(*entry, std::move(settings));
  }
  profile.sanitize();
  expect(entry && profile.entries.contains(*entry) &&
             profile.entries.at(*entry).options.size() == 1 &&
             profile.entries.at(*entry).options.begin()->first.size() ==
                 authoredTextBytes,
         "profile persistence retains every valid Beatoraja configuration "
         "key without an app-defined text limit");
}

void testDuplicateCustomFilesReuseTheirPersistedSelection() {
  const auto duplicate = fixture().decode("duplicate-file-name.luaskin");
  auto fileSystem = fixture().fileSystem("duplicate-file-name.luaskin");
  expect(duplicate.header && fileSystem,
         "duplicate-file reconciliation fixture is available");
  if (!duplicate.header || !fileSystem) {
    return;
  }
  EntryProfileSettings saved;
  saved.filePaths.emplace("Reused", "persisted.png");
  const auto reconciled =
      reconcileSkinConfiguration(*duplicate.header, &saved, *fileSystem);
  expect(
      reconciled.configuration && reconciled.diagnostics.empty() &&
          reconciled.reconciledSettings.filePaths ==
              std::map<std::string, std::string>{{"Reused", "persisted.png"}} &&
          reconciled.configuration->orderedFiles.size() == 2 &&
          reconciled.configuration->orderedFiles[0].selectedValue ==
              "persisted.png" &&
          reconciled.configuration->orderedFiles[1].selectedValue ==
              "persisted.png",
      "duplicate custom-file names share the stored selection exactly as "
      "Beatoraja's SkinConfig does");
}

void testUnresolvedHeaderConfigurationRemainsSelectable() {
  const auto decoded = fixture().decode("empty-choices.luaskin");
  auto choices = fixture().fileSystem("empty-choices.luaskin");
  expect(decoded.header && choices,
         "empty-option reconciliation fixture is available");
  if (!decoded.header || !choices) {
    return;
  }
  const auto emptyOption =
      reconcileSkinConfiguration(*decoded.header, nullptr, *choices);
  expect(emptyOption.configuration && emptyOption.diagnostics.empty() &&
             emptyOption.configuration->options ==
                 std::map<std::string, int>{{"Unconfigured", -1}} &&
             emptyOption.configuration->enabledOptionIds == std::set<int>{-1} &&
             emptyOption.reconciledSettings.options ==
                 std::map<std::string, int>{{"Unconfigured", -1}},
         "an option with no choices exports Beatoraja's random sentinel "
         "as its stable effective configuration");

  const auto unresolved = fixture().decode("unresolved-file.luaskin");
  auto files = fixture().fileSystem("unresolved-file.luaskin");
  expect(unresolved.header && files,
         "unresolved-file reconciliation fixture is available");
  if (!unresolved.header || !files) {
    return;
  }
  const auto missingFile =
      reconcileSkinConfiguration(*unresolved.header, nullptr, *files);
  expect(
      missingFile.configuration && missingFile.diagnostics.empty() &&
          missingFile.configuration->filePaths.empty() &&
          missingFile.reconciledSettings.filePaths.empty() &&
          missingFile.configuration->orderedFiles.size() == 1 &&
          missingFile.configuration->orderedFiles.front().selectedValue.empty(),
      "a missing custom-file directory remains unconfigured instead of "
      "making the skin unselectable");
}

void testDuplicateFontNamesUseTheFirstBeatorajaDefinition() {
  const auto decoded = fixture().decodeGameplay("duplicate-font.luaskin");
  const auto *caption =
      decoded.model ? objectNamed(*decoded.model, "caption") : nullptr;
  const auto *text =
      caption ? std::get_if<SkinTextObject>(&caption->payload) : nullptr;
  expect(decoded.model && decoded.diagnostics.empty() && text != nullptr &&
             text->font == SkinResourceId{1},
         "duplicate font names use the first Beatoraja definition instead of "
         "rejecting the skin");
}

void testResourcePathsFollowBeatorajaResolution() {
  const auto decoded =
      fixture().decodeGameplay("external-resource-paths.luaskin");
  const auto *caption =
      decoded.model ? objectNamed(*decoded.model, "caption") : nullptr;
  const auto *text =
      caption ? std::get_if<SkinTextObject>(&caption->payload) : nullptr;
  expect(decoded.model && decoded.diagnostics.empty() && text != nullptr &&
             text->font == SkinResourceId{2},
         "authored resource paths reach Beatoraja-style resolution");
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
                 std::map<std::string, std::string>{
                     {"Background", "missing.png"}} &&
             configuration.filePaths.at("Background") == "missing.png",
         "a persisted Beatoraja custom-file value remains selected even when "
         "the current package no longer contains it");
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

void testRandomOptionKeepsPersistedSentinelAndExportsAnAuthoredChoice() {
  const auto decoded = fixture().decode("valid.luaskin");
  auto fileSystem = fixture().fileSystem();
  expect(decoded.header && fileSystem,
         "random-option reconciliation fixture is available");
  if (!decoded.header || !fileSystem) {
    return;
  }

  EntryProfileSettings saved;
  saved.options = {{"Lane type", -1}, {"Gauge", 11}};
  const auto reconciled =
      reconcileSkinConfiguration(*decoded.header, &saved, *fileSystem);
  if (!reconciled.configuration) {
    expect(false, "random custom option reconciles successfully");
    return;
  }

  const auto &configuration = *reconciled.configuration;
  const auto lane = std::find_if(
      configuration.orderedOptions.begin(), configuration.orderedOptions.end(),
      [](const auto &option) { return option.name == "Lane type"; });
  expect(reconciled.diagnostics.empty(),
         "a saved Beatoraja Random option reconciles without diagnostics");
  expect(reconciled.reconciledSettings.options == saved.options,
         "a saved Beatoraja Random option preserves -1 in profile state");
  expect(configuration.options == saved.options,
         "the runtime digest state preserves the saved random sentinel");
  expect(lane != configuration.orderedOptions.end() &&
             (lane->value == 927 || lane->value == 928),
         "configured Lua receives a fresh authored option");
  if (lane == configuration.orderedOptions.end()) {
    return;
  }
  expect(configuration.enabledOptionIds.contains(lane->value),
         "the fresh authored option enables its Beatoraja condition ID");
  expect(configuration.lowercaseSha256 ==
             skinConfigurationDigest(reconciled.reconciledSettings),
         "the persistent random sentinel owns the configuration digest");
}

void testPinnedRandomRuntimeSelectionReusesMaterializedChoices() {
  const auto decoded = fixture().decode("valid.luaskin");
  auto fileSystem = fixture().fileSystem();
  expect(decoded.header && fileSystem,
         "pinned random reconciliation fixture is available");
  if (!decoded.header || !fileSystem) {
    return;
  }

  EntryProfileSettings saved;
  saved.options = {{"Lane type", -1}, {"Gauge", 11}};
  saved.filePaths = {{"Background", "Random"}};
  const auto first =
      reconcileSkinConfiguration(*decoded.header, &saved, *fileSystem);
  if (!first.configuration) {
    expect(false, "initial random reconciliation succeeds");
    return;
  }
  const auto pinned = runtimeSkinConfigurationSelection(*first.configuration);
  const auto second = reconcileSkinConfiguration(*decoded.header, &saved,
                                                  *fileSystem, &pinned);
  const auto selectedOption = [](const auto &options, std::string_view name) {
    return std::find_if(options.begin(), options.end(),
                        [name](const auto &option) {
                          return option.name == name;
                        });
  };
  const auto firstLane = selectedOption(pinned.orderedOptions, "Lane type");
  const auto secondLane = second.configuration
                              ? selectedOption(second.configuration->orderedOptions,
                                               "Lane type")
                              : std::vector<ConfiguredOption>::const_iterator{};
  const auto firstBackground =
      selectedOption(pinned.orderedFiles, "Background");
  const auto secondBackground =
      second.configuration
          ? selectedOption(second.configuration->orderedFiles, "Background")
          : std::vector<ConfiguredFile>::const_iterator{};
  expect(second.configuration && second.diagnostics.empty() &&
             second.reconciledSettings.options == saved.options &&
             second.reconciledSettings.filePaths == saved.filePaths &&
             firstLane != pinned.orderedOptions.end() &&
             secondLane != second.configuration->orderedOptions.end() &&
             secondLane->value == firstLane->value &&
             firstBackground != pinned.orderedFiles.end() &&
             secondBackground != second.configuration->orderedFiles.end() &&
             secondBackground->selectedValue == firstBackground->selectedValue,
         "a course render reuses the preflight materialization behind persisted Random sentinels");
}

void testReconciliationRejectsEmptyConfigurationKeys() {
  auto fileSystem = fixture().fileSystem("optional-text.luaskin");
  expect(fileSystem != nullptr, "empty-key reconciliation filesystem exists");
  if (!fileSystem) {
    return;
  }

  BeatorajaSkinHeader header;
  header.type = 5;
  header.options = {{.name = "", .choices = {{.label = "Value", .value = 1}}}};
  auto option = reconcileSkinConfiguration(header, nullptr, *fileSystem);
  expect(!option.configuration && !option.diagnostics.empty(),
         "empty option names never become configuration keys");

  header.options.clear();
  header.files = {{.name = "", .pattern = "ordinary/*.png"}};
  auto file = reconcileSkinConfiguration(header, nullptr, *fileSystem);
  expect(!file.configuration && !file.diagnostics.empty(),
         "empty file names never become configuration keys");

  header.files.clear();
  header.offsets = {{.name = "", .id = 100}};
  auto offset = reconcileSkinConfiguration(header, nullptr, *fileSystem);
  expect(!offset.configuration && !offset.diagnostics.empty(),
         "empty offset names never become configuration keys");
}

void testPinnedFilePatternChoicesAreDeterministicAndCaseInsensitive() {
  const auto decoded = fixture().decode("patterns.luaskin");
  auto fileSystem = fixture().fileSystem("patterns.luaskin");
  expect(decoded.header && fileSystem,
         "file-pattern reconciliation fixture is available");
  if (!decoded.header || !fileSystem) {
    return;
  }

  const struct {
    std::size_t index;
    std::string_view expected;
  } cases[] = {
      {0, "plain.png"},
      {1, "Cover.PNG"},
      {2, "Alpha.webp"},
      {3, "Portrait.PNG"},
  };
  for (const auto &[index, expected] : cases) {
    BeatorajaSkinHeader header = *decoded.header;
    header.files = {decoded.header->files.at(index)};
    const auto reconciled =
        reconcileSkinConfiguration(header, nullptr, *fileSystem);
    expect(reconciled.configuration && reconciled.diagnostics.empty() &&
               reconciled.configuration->filePaths ==
                   std::map<std::string, std::string>{
                       {header.files[0].name, std::string(expected)}},
           "ordinary, uppercase, and pinned alternative patterns reconcile");
  }

  BeatorajaSkinHeader randomHeader = *decoded.header;
  randomHeader.files = {decoded.header->files.front()};
  EntryProfileSettings savedRandom;
  savedRandom.filePaths.emplace("Ordinary", "Random");
  const auto random =
      reconcileSkinConfiguration(randomHeader, &savedRandom, *fileSystem);
  expect(random.configuration && random.diagnostics.empty() &&
             random.reconciledSettings.filePaths == savedRandom.filePaths &&
             random.configuration->orderedFiles.size() == 1 &&
             random.configuration->orderedFiles.front().selectedValue ==
                 "plain.png",
         "a persisted Random custom-file choice stays exported as Random while "
         "the loader receives one matching file");
}

void testEntryRelativeFilePatternsStayWithinThePackage() {
  const auto decoded = fixture().decode("system/relative-file-pattern.luaskin");
  auto fileSystem =
      fixture().fileSystem("system/relative-file-pattern.luaskin");
  expect(decoded.header && fileSystem,
         "entry-relative file pattern fixture is available");
  if (!decoded.header || !fileSystem) {
    return;
  }
  const auto reconciled =
      reconcileSkinConfiguration(*decoded.header, nullptr, *fileSystem);
  expect(
      reconciled.configuration && reconciled.diagnostics.empty() &&
          reconciled.configuration->filePaths ==
              std::map<std::string, std::string>{{"Settings", "default.lua"}},
      "a custom file pattern may traverse to the entry's package parent");

  const auto escaped = fixture().decode("system/escaping-file-pattern.luaskin");
  auto escapedFileSystem =
      fixture().fileSystem("system/escaping-file-pattern.luaskin");
  expect(escaped.header && escapedFileSystem,
         "escaping file pattern fixture reaches reconciliation");
  if (!escaped.header || !escapedFileSystem) {
    return;
  }
  const auto unresolved =
      reconcileSkinConfiguration(*escaped.header, nullptr, *escapedFileSystem);
  expect(unresolved.configuration && unresolved.diagnostics.empty() &&
             unresolved.configuration->filePaths.empty(),
         "a header-only catalog pass leaves an unresolved custom-file path "
         "unconfigured; actual resource access remains sandboxed later");
}

void testRepeatedOffsetIdsFollowBeatorajaLastValueSemantics() {
  const auto decoded = fixture().decode("duplicate-offset-id.luaskin");
  auto fileSystem = fixture().fileSystem("duplicate-offset-id.luaskin");
  expect(decoded.header && fileSystem,
         "duplicate offset ID fixture is available");
  if (!decoded.header || !fileSystem) {
    return;
  }
  EntryProfileSettings saved;
  saved.offsets.emplace("First", ConfigOffset{.x = 7});
  saved.offsets.emplace("Second", ConfigOffset{.y = 9});
  const auto reconciled =
      reconcileSkinConfiguration(*decoded.header, &saved, *fileSystem);
  expect(reconciled.configuration && reconciled.diagnostics.empty() &&
             reconciled.configuration->offsets.at("First") ==
                 ConfigOffset{.x = 7} &&
             reconciled.configuration->offsets.at("Second") ==
                 ConfigOffset{.y = 9} &&
             reconciled.configuration->offsetsById.at(120) ==
                 ConfigOffset{.y = 9},
         "repeated offset IDs retain both settings and use the final value at "
         "runtime");
}

void testUnrelatedDirectoryEntriesDoNotConsumeTheChoiceLimit() {
  const auto decoded = fixture().decode("crowded.luaskin");
  auto fileSystem = fixture().fileSystem("crowded.luaskin");
  expect(decoded.header && fileSystem,
         "crowded-directory reconciliation fixture is available");
  if (!decoded.header || !fileSystem) {
    return;
  }
  const auto reconciled =
      reconcileSkinConfiguration(*decoded.header, nullptr, *fileSystem);
  expect(reconciled.configuration && reconciled.diagnostics.empty() &&
             reconciled.configuration->filePaths ==
                 std::map<std::string, std::string>{{"Crowded", "match.png"}},
         "unrelated directory entries do not consume the matching choice cap");
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
  expect(skinConfigurationDigest(BeatorajaSkinConfiguration{}) ==
             "f3c2c52f1de34a366df4f5bad4eb6a5bc080153949ea6422cb81aebfc84bc4b3",
         "empty configuration digest matches the frozen vector");
}

const SkinObjectDefinition *objectNamed(const BeatorajaSkinModel &model,
                                        std::string_view name) {
  const auto found =
      std::ranges::find_if(model.objects, [&](const auto &object) {
        return object.authoredName == name;
      });
  return found != model.objects.end() ? &*found : nullptr;
}

void testGameplayNumericGlyphAtlasesNormalizeIntoModelObjects() {
  const auto decoded = fixture().decodeGameplay("numeric-glyphs.luaskin");
  expect(decoded.model.has_value() && decoded.diagnostics.empty(),
         "numeric glyph gameplay model decodes");
  if (!decoded.model) {
    return;
  }

  const auto *signedNumber = objectNamed(*decoded.model, "signed-number");
  const auto *plainNumber = objectNamed(*decoded.model, "plain-number");
  const auto *signedFloat = objectNamed(*decoded.model, "signed-float");
  expect(signedNumber != nullptr && plainNumber != nullptr &&
             signedFloat != nullptr,
         "all numeric destinations materialize objects");
  if (!signedNumber || !plainNumber || !signedFloat) {
    return;
  }

  const auto *number = std::get_if<SkinNumberObject>(&signedNumber->payload);
  const auto *plain = std::get_if<SkinNumberObject>(&plainNumber->payload);
  const auto *floating = std::get_if<SkinFloatObject>(&signedFloat->payload);
  expect(number != nullptr && plain != nullptr && floating != nullptr,
         "numeric objects retain their typed payloads");
  if (!number || !plain || !floating) {
    return;
  }

  expect(number->digits.glyphsPerAnimationFrame == 12 &&
             number->digits.positive.frames.size() == 24 &&
             number->digits.negative &&
             number->digits.negative->frames.size() == 24 &&
             number->digits.positive.frames[12].x == 0 &&
             number->digits.positive.frames[12].w == 480 &&
             number->digits.positive.frames[12].gridColumn == 24 &&
             number->digits.positive.frames[12].gridColumns == 48 &&
             number->digits.negative->frames[12].x == 0 &&
             number->digits.negative->frames[12].w == 480 &&
             number->digits.negative->frames[12].gridColumn == 36 &&
             number->digits.negative->frames[12].gridColumns == 48,
         "Number-24 partitions both signed multi-frame glyph sets without "
         "pre-resolving texture grids");
  expect(number->digits.positive.resource ==
                 number->digits.negative->resource &&
             number->digits.positive.timer == number->digits.negative->timer &&
             number->digits.positive.cycleMillis == 240 &&
             number->digits.negative->cycleMillis == 240,
         "Number partition preserves resource, timer, and cycle");
  expect(number->digitCount == 12 && number->spacing == 3 &&
             number->alignment == 2 &&
             number->zeroPadding == SkinZeroPaddingMode::AlternateZero &&
             number->perDigitOffsets.size() == 2 &&
             number->perDigitOffsets[0].x == 1.5 &&
             number->perDigitOffsets[0].y == -2.5 &&
             number->perDigitOffsets[0].width == 3.5 &&
             number->perDigitOffsets[0].height == 4.5 &&
             number->perDigitOffsets[1].width == -7.5,
         "Number maps normalized format and every offset component");
  expect(plain->digits.glyphsPerAnimationFrame == 10 &&
             !plain->digits.negative &&
             plain->zeroPadding == SkinZeroPaddingMode::Zero,
         "Number-10 padding takes precedence from padding over zeropadding");

  expect(floating->digits.glyphsPerAnimationFrame == 12 &&
             floating->digits.positive.frames.size() == 24 &&
             floating->digits.negative &&
             floating->digits.negative->frames.size() == 24 &&
             floating->digits.positive.frames[10].x == 0 &&
             floating->digits.positive.frames[10].w == 440 &&
             floating->digits.positive.frames[10].gridColumn == 0 &&
             floating->digits.negative->frames[10].gridColumn == 11 &&
             floating->digits.positive.frames[11].gridColumn == 10 &&
             floating->digits.negative->frames[11].gridColumn == 21 &&
             floating->digits.positive.frames[12].gridColumn == 22 &&
             floating->digits.negative->frames[12].gridColumn == 33 &&
             floating->digits.positive.frames[12].gridColumns == 44 &&
             floating->digits.negative->frames[12].gridColumns == 44,
         "Float-22 duplicates signed reverse-zero and preserves row order "
         "without pre-resolving texture grids");
  expect(floating->integerDigits == 3 && floating->fractionalDigits == 5 &&
             floating->zeroPadding == SkinZeroPaddingMode::Zero &&
             !floating->signVisible && floating->gain == 1.25 &&
             floating->spacing == 4 && floating->perDigitOffsets.size() == 1 &&
             floating->perDigitOffsets.front().x == 9.5 &&
             floating->perDigitOffsets.front().height == 12.5,
         "Float maps normalized digits, sign, padding, gain, and offsets");
  expect(floating->digits.positive.resource ==
                 floating->digits.negative->resource &&
             floating->digits.positive.timer ==
                 floating->digits.negative->timer &&
             floating->digits.positive.cycleMillis == 360 &&
             floating->digits.negative->cycleMillis == 360,
         "Float partition preserves resource, timer, and cycle");
}

void testGameplayNumericOffsetsAndCumulativeFrameBudgetAreBounded() {
  const auto offsets = fixture().decodeGameplay("numeric-offset-limit.luaskin");
  expect(!offsets.model && !offsets.diagnostics.empty(),
         "numeric offset arrays reject above the established maxOffsets bound");
  const auto nonFinite =
      fixture().decodeGameplay("numeric-offset-nonfinite.luaskin");
  expect(!nonFinite.model && !nonFinite.diagnostics.empty(),
         "numeric offsets reuse finite-number decoding for every component");

  const auto boundary =
      fixture().decodeGameplay("numeric-budget-boundary.luaskin");
  expect(boundary.model.has_value(),
         "normalized numeric output may consume the exact remaining model "
         "frame budget");

  const auto exceeded =
      fixture().decodeGameplay("numeric-budget-exceeded.luaskin");
  expect(!exceeded.model && !exceeded.diagnostics.empty() &&
             exceeded.diagnostics.front().code ==
                 "skin_lua_model_limit_exceeded",
         "Float-22 expansion is checked against the cumulative remaining "
         "model frame budget");
}

void testRequestedExternalLuaSkinHeaderDecodes() {
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
      configuredEntry != nullptr && *configuredEntry != '\0' ? configuredEntry
                                                             : "result.luaskin";

  TempDirectory temp;
  const fs::path projectedSource = temp.root() / "source";
  copyLuaSources(source, projectedSource);
  const auto package = normalizePackageId("ExternalLuaSkin").package;
  const auto entry =
      package ? normalizeEntryPath(*package, entryPath).entry : std::nullopt;
  expect(package.has_value() && entry.has_value(),
         "requested external Lua entry has a portable virtual identity");
  if (!package || !entry) {
    return;
  }

  const SkinStorageRoots roots{.visiblePackages = temp.root() / "visible",
                               .privateRevisions = temp.root() / "revisions",
                               .privateCatalog = temp.root() / "catalog",
                               .profileOverlays = temp.root() / "overlays"};
  AcceptFiles aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  const auto snapshot = snapshotter.snapshot(projectedSource, *package, {}, {});
  expect(snapshot.prepared.has_value(),
         "requested external Lua source projection snapshots");
  if (!snapshot.prepared) {
    return;
  }
  auto fileSystem =
      LuaSkinFileSystem::create({.revision = snapshot.prepared->readView(),
                                 .entry = *entry,
                                 .storageRoots = roots});
  expect(fileSystem.fileSystem != nullptr,
         "requested external Lua decoder filesystem is created");
  if (!fileSystem.fileSystem) {
    return;
  }
  auto runtime =
      LuaSkinRuntime::create({.purpose = LuaRuntimePurpose::Catalog,
                              .fileSystem = std::move(fileSystem.fileSystem)});
  expect(runtime.runtime != nullptr,
         "requested external Lua decoder runtime is created");
  if (!runtime.runtime) {
    return;
  }
  const auto header = runtime.runtime->loadHeader();
  expect(header.value.has_value() && !header.failure,
         "requested external Lua header executes before decoding");
  if (!header.value) {
    return;
  }
  const auto decoded = LuaSkinTableDecoder{}.decodeHeader(*header.value);
  for (const auto &diagnostic : decoded.diagnostics) {
    std::cerr << "external Lua header decode diagnostic: " << diagnostic.code
              << ": " << diagnostic.message << '\n';
  }
  expect(
      decoded.header.has_value() && decoded.diagnostics.empty(),
      "requested external Lua header decodes through the compatibility model");
}

} // namespace

int main() {
  testTypedHeaderPreservesAuthoredNumericOrderAndCoercions();
  testHeaderArraysFollowBeatorajaTableKeys();
  testAuthoredDimensionsStayWithinTheDecoderBoundary();
  testHeaderTextFollowsLuaValueCoercion();
  testHeaderPreservesBeatorajaConfigurationDeclarations();
  testHeaderDoesNotImposeAnUnpinnedTextLimit();
  testHeaderAndConfigurationAcceptLongBeatorajaNames();
  testHeaderAndProfileConfigurationDoNotCapAuthoredText();
  testDuplicateCustomFilesReuseTheirPersistedSelection();
  testUnresolvedHeaderConfigurationRemainsSelectable();
  testDuplicateFontNamesUseTheFirstBeatorajaDefinition();
  testResourcePathsFollowBeatorajaResolution();
  testReconciliationDefaultsSanitizesAndIndexesConfiguration();
  testRandomOptionKeepsPersistedSentinelAndExportsAnAuthoredChoice();
  testPinnedRandomRuntimeSelectionReusesMaterializedChoices();
  testReconciliationRejectsEmptyConfigurationKeys();
  testPinnedFilePatternChoicesAreDeterministicAndCaseInsensitive();
  testEntryRelativeFilePatternsStayWithinThePackage();
  testRepeatedOffsetIdsFollowBeatorajaLastValueSemantics();
  testUnrelatedDirectoryEntriesDoNotConsumeTheChoiceLimit();
  testConfigurationDigestUsesTheFrozenBigEndianGrammar();
  testGameplayNumericGlyphAtlasesNormalizeIntoModelObjects();
  testGameplayNumericOffsetsAndCumulativeFrameBudgetAreBounded();
  testRequestedExternalLuaSkinHeaderDecodes();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "lua skin table decoder tests passed\n";
  return 0;
}
