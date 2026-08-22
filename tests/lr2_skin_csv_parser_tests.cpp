#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/Lr2SkinCsvParser.h"
#include "gameplay_skin_ledger_evidence.h"
#include "skin/beatoraja/Lr2SkinHeaderDecoder.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
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

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-lr2-parser-test-" + std::to_string(++serial));
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

class PackageFixture {
public:
  explicit PackageFixture(std::string_view entryPath)
      : roots(rootsBelow(temp.root())),
        package(*normalizePackageId("FixtureSkin").package),
        entry(*normalizeEntryPath(package, entryPath).entry) {
    const fs::path source = temp.root() / "source";
    const fs::path committed = fs::path(ASOBMASHOW_SOURCE_DIR) /
                               "tests/fixtures/beatoraja_skin/lr2";
    fs::copy(committed, source, fs::copy_options::recursive);

    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto snapshot = snapshotter.snapshot(source, package, {}, {});
    expect(snapshot.prepared.has_value(), "LR2 fixture package snapshots");
    if (snapshot.prepared) prepared.emplace(std::move(*snapshot.prepared));

    auto created = LuaSkinFileSystem::create(
        {.revision = prepared->readView(),
         .entry = entry,
         .storageRoots = roots,
         .profileId = std::nullopt,
         .allowDataWrites = false});
    expect(created.fileSystem != nullptr && !created.failure,
           "LR2 fixture package filesystem opens");
    fileSystem = std::move(created.fileSystem);
  }

  SkinFileReadResult readEntry() const {
    return fileSystem->readEntry(Lr2SkinCsvParserLimits::maxDocumentBytes);
  }

  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageId package;
  SkinEntryId entry;
  AcceptFiles aliases;
  std::optional<PreparedSkinRevision> prepared;
  std::unique_ptr<LuaSkinFileSystem> fileSystem;
};

bool hasDiagnostic(const Lr2SkinParseResult &result, std::string_view code) {
  return std::ranges::any_of(result.diagnostics, [&](const auto &diagnostic) {
    return diagnostic.code == code;
  });
}

bool sourceEquals(const SkinSourceLocation &source, std::string_view path,
                  std::uint32_t line, std::uint32_t column) {
  return source.virtualPath == path && source.line == line &&
         source.column == column;
}

const SkinDiagnostic *diagnostic(const Lr2SkinParseResult &result,
                                 std::string_view code) {
  const auto found = std::ranges::find_if(
      result.diagnostics,
      [&](const auto &candidate) { return candidate.code == code; });
  return found == result.diagnostics.end() ? nullptr : &*found;
}

std::vector<const Lr2SkinCommand *>
commandsNamed(const Lr2SkinParseResult &result, std::string_view name) {
  std::vector<const Lr2SkinCommand *> commands;
  for (const auto &command : result.commands) {
    if (command.name == name) commands.push_back(&command);
  }
  return commands;
}

void testCp932TokenizationIncludesAndHeader() {
  PackageFixture fixture("header/main.lr2skin");
  const auto bytes = fixture.readEntry();
  expect(!bytes.failure && !bytes.bytes.empty(), "CP932 entry reads bounded");

  const auto parsed = Lr2SkinCsvParser{}.parse(
      *fixture.fileSystem, fixture.entry.packageRelativePath, bytes.bytes, {});
  expect(!parsed.cancelled && parsed.diagnostics.empty(),
         "valid CP932 include tree parses without diagnostics");

  const auto images = commandsNamed(parsed, "IMAGE");
  const std::vector<std::string> expectedImages{
      "root-before.png", "included-before.png", "nested.png",
      "included-after.png", "root-after.png", "\"quoted"};
  expect(images.size() == expectedImages.size(),
         "only column-one hash commands enter the stream");
  for (std::size_t index = 0;
       index < images.size() && index < expectedImages.size(); ++index) {
    expect(!images[index]->fields.empty() &&
               images[index]->fields.front() == expectedImages[index],
           "includes are inserted in exact authored order");
  }

  expect(images.size() >= 6 && images[4]->fields.size() == 3 &&
             images[4]->fields[1].empty() && images[4]->fields[2].empty(),
         "literal comma splitting preserves empty and trailing fields");
  expect(images.size() >= 6 && images[5]->fields.size() == 3 &&
             images[5]->fields[0] == "\"quoted" &&
             images[5]->fields[1] == "comma.png\"" &&
             images[5]->fields[2] == "tail",
         "quotes have no special meaning in pinned LR2 tokenization");

  expect(images.size() >= 4 &&
             sourceEquals(images[0]->source, "header/main.lr2skin", 11, 1) &&
             sourceEquals(images[2]->source, "header/nested.lr2skin", 1, 1),
         "commands retain exact package-relative source file and line");
  expect(images.size() >= 4 &&
             images[0]->includeChain ==
                 std::vector<std::string>{"header/main.lr2skin"} &&
             images[2]->includeChain ==
                 (std::vector<std::string>{"header/main.lr2skin",
                                           "header/included.lr2skin",
                                           "header/nested.lr2skin"}),
         "commands retain the full root-to-source include chain");

  const auto headerResult = Lr2SkinHeaderDecoder{}.decode(parsed.commands);
  expect(headerResult.header.has_value() && headerResult.diagnostics.empty(),
         "pinned LR2 header commands decode");
  if (!headerResult.header) return;
  const auto &header = *headerResult.header;
  expect(header.type == 0 && header.width == 1280 && header.height == 720 &&
             header.name == "日本語スキン" && header.author == "作者",
         "CP932 header text and HD coordinate metadata decode exactly");
  expect(header.options.size() == 5 &&
             header.options[0].name == "Ghost" &&
             header.options[1].name == "Judge Detail" &&
             header.options[2].name == "表示" &&
             header.options[2].choices.size() == 2 &&
             header.options[2].choices[0].label == "標準" &&
             header.options[2].choices[0].value == 900 &&
             header.options[2].choices[1].label == "拡張" &&
             header.options[2].choices[1].value == 901 &&
             header.options[3].name == "重複" &&
             header.options[4].name == "重複",
         "built-in removal, empty choices, and duplicate options match the pinned loader");
  expect(header.files.size() == 1 && header.files[0].name == "画像" &&
             header.files[0].pattern ==
                 "skin/FixtureSkin/header/*.png" &&
             header.files[0].defaultValue == "既定.png",
         "custom-file paths use the pinned LR2 theme substitution");
  expect(header.offsets.size() == 5 && header.offsets.back().name == "移動" &&
             header.offsets.back().id == 77 &&
             header.offsets.back().permissions ==
                 (kOffsetPermissionX | kOffsetPermissionW |
                  kOffsetPermissionH | kOffsetPermissionR |
                  kOffsetPermissionA),
         "missing custom-offset flags default true and duplicate declarations append");
  expect(header.width != 3840,
         "header INCLUDE remains a no-op for included RESOLUTION declarations");
}

void testPinnedDefaults() {
  const auto decoded = Lr2SkinHeaderDecoder{}.decode(
      std::span<const Lr2SkinCommand>{});
  expect(decoded.header.has_value() && decoded.diagnostics.empty(),
         "an empty LR2 header retains the source defaults");
  if (!decoded.header) return;
  expect(decoded.header->type == -1 && decoded.header->width == 640 &&
             decoded.header->height == 480 && decoded.header->name.empty() &&
             decoded.header->author.empty() && decoded.header->options.empty() &&
             decoded.header->files.empty() && decoded.header->offsets.empty(),
         "LR2 header defaults are null mode, SD coordinates, and empty declarations");
}

void testCycleGuardRetainsSafeSiblings() {
  PackageFixture fixture("header/cycle-a.lr2skin");
  const auto bytes = fixture.readEntry();
  const auto parsed = Lr2SkinCsvParser{}.parse(
      *fixture.fileSystem, fixture.entry.packageRelativePath, bytes.bytes, {});
  const auto images = commandsNamed(parsed, "IMAGE");
  const std::vector<std::string> expected{
      "cycle-a-before.png", "cycle-b-before.png", "cycle-b-after.png",
      "cycle-a-after.png"};
  expect(images.size() == expected.size(),
         "a cyclic include rejects only the recursive branch");
  for (std::size_t index = 0;
       index < images.size() && index < expected.size(); ++index) {
    expect(images[index]->fields.front() == expected[index],
           "valid siblings retain order around a rejected cycle");
  }
  const auto *cycle = diagnostic(parsed, "skin_lr2_include_cycle");
  expect(cycle != nullptr &&
             cycle->message.find(
                 "header/cycle-a.lr2skin -> header/cycle-b.lr2skin -> "
                 "header/cycle-a.lr2skin") != std::string::npos &&
             cycle->source &&
             sourceEquals(*cycle->source, "header/cycle-b.lr2skin", 2, 1),
         "cycle diagnostics carry the full include chain and source line");
}

void testDepthByteEncodingAndCancellationGuards() {
  PackageFixture fixture("header/main.lr2skin");
  const auto bytes = fixture.readEntry();

  Lr2SkinCsvParser shallow({.maximumDocumentBytes =
                                Lr2SkinCsvParserLimits::maxDocumentBytes,
                            .maximumIncludeDepth = 2});
  const auto depth = shallow.parse(*fixture.fileSystem,
                                   fixture.entry.packageRelativePath,
                                   bytes.bytes, {});
  expect(hasDiagnostic(depth, "skin_lr2_include_depth") &&
             std::ranges::any_of(depth.commands, [](const auto &command) {
               return command.name == "IMAGE" && !command.fields.empty() &&
                      command.fields.front() == "root-after.png";
             }),
         "depth overflow rejects only the unsafe nested include");

  Lr2SkinCsvParser byteLimited(
      {.maximumDocumentBytes = bytes.bytes.size() + 1,
       .maximumIncludeDepth = Lr2SkinCsvParserLimits::maxIncludeDepth});
  const auto limited = byteLimited.parse(*fixture.fileSystem,
                                         fixture.entry.packageRelativePath,
                                         bytes.bytes, {});
  expect(hasDiagnostic(limited, "skin_lr2_byte_limit") &&
             std::ranges::any_of(limited.commands, [](const auto &command) {
               return command.name == "IMAGE" && !command.fields.empty() &&
                      command.fields.front() == "root-after.png";
             }),
         "aggregate byte overflow rejects only the oversized include branch");

  const std::array<std::byte, 1> invalid{std::byte{0x81}};
  const auto invalidEncoding = Lr2SkinCsvParser{}.parse(
      *fixture.fileSystem, fixture.entry.packageRelativePath, invalid, {});
  expect(hasDiagnostic(invalidEncoding, "skin_lr2_encoding_invalid") &&
             invalidEncoding.commands.empty(),
         "invalid CP932 is diagnosed without partially decoded commands");

  std::stop_source source;
  source.request_stop();
  const auto cancelled = Lr2SkinCsvParser{}.parse(
      *fixture.fileSystem, fixture.entry.packageRelativePath, bytes.bytes,
      source.get_token());
  expect(cancelled.cancelled && cancelled.commands.empty() &&
             cancelled.diagnostics.empty(),
         "pre-requested cancellation is distinct from invalid input");
}

void testFalseConditionDoesNotReadOrDiagnoseIncludes() {
  PackageFixture fixture("header/skipped-includes.lr2skin");
  const auto bytes = fixture.readEntry();
  const auto parsed = Lr2SkinCsvParser{}.parse(
      *fixture.fileSystem, fixture.entry.packageRelativePath, bytes.bytes, {},
      {.includeExpansion = Lr2IncludeExpansionMode::ConditionAware});
  const auto images = commandsNamed(parsed, "IMAGE");
  expect(!parsed.cancelled && parsed.diagnostics.empty() &&
             images.size() == 1 && !images.front()->fields.empty() &&
             images.front()->fields.front() == "after-skipped-includes.png",
         "a false IF neither reads nor diagnoses missing/cyclic includes and "
         "retains following commands");
}

void testMalformedSetOptionCannotActivateIncludes() {
  PackageFixture fixture("header/malformed-setoption.lr2skin");
  const auto bytes = fixture.readEntry();
  const auto parsed = Lr2SkinCsvParser{}.parse(
      *fixture.fileSystem, fixture.entry.packageRelativePath, bytes.bytes, {},
      {.includeExpansion = Lr2IncludeExpansionMode::ConditionAware});
  const auto sources = commandsNamed(parsed, "SRC_IMAGE");
  const auto options = commandsNamed(parsed, "SETOPTION");
  expect(!parsed.cancelled && !parsed.fatal && parsed.diagnostics.empty() &&
             sources.size() == 3 && options.size() == 3 &&
             options[0]->fields.front() == "id=777" &&
             options[1]->fields[1] == "1junk" &&
             options[2]->fields.front() == "+778",
         "malformed root and included SETOPTION values neither mutate the "
         "include fold nor read missing/unsafe branches, while Java-valid "
         "plus signs and following commands survive");
}

} // namespace

int main(int argc, char **argv) {
  testCp932TokenizationIncludesAndHeader();
  testPinnedDefaults();
  testCycleGuardRetainsSafeSiblings();
  testDepthByteEncodingAndCancellationGuards();
  testFalseConditionDoesNotReadOrDiagnoseIncludes();
  testMalformedSetOptionCannotActivateIncludes();

  return gameplay_skin_ledger_evidence::finish(
      argc, argv, "lr2_skin_csv_parser_tests", failures,
      "LR2 skin parser test(s) failed", "LR2 skin CSV parser tests passed");
}
