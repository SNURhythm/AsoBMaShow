#include "skin/package/SkinArchiveImporter.h"
#include "skin/package/SkinPathPolicy.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <Aclapi.h>
#include <Windows.h>
#endif

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
    root_ = fs::canonical(fs::temp_directory_path()) /
            ("asobmashow-skin-import-test-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++serial));
    fs::create_directories(root_);
  }
  ~TempDirectory() {
    std::error_code ignored;
    fs::permissions(root_, fs::perms::owner_all, fs::perm_options::add,
                    ignored);
    for (fs::recursive_directory_iterator iterator(root_, ignored), end;
         !ignored && iterator != end; ++iterator) {
      if (iterator->is_directory(ignored)) {
        fs::permissions(iterator->path(), fs::perms::owner_all,
                        fs::perm_options::add, ignored);
      }
    }
    ignored.clear();
    fs::remove_all(root_, ignored);
  }
  const fs::path &root() const { return root_; }

private:
  fs::path root_;
};

class NoAliases final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class ImportIoObserver final : public SkinImportIoObserver {
public:
  explicit ImportIoObserver(
      std::function<void(SkinImportIoOperation, const fs::path &)> callback)
      : callback_(std::move(callback)) {}

  void reached(SkinImportIoOperation operation,
               const fs::path &path) const override {
    callback_(operation, path);
  }

private:
  std::function<void(SkinImportIoOperation, const fs::path &)> callback_;
};

struct ZipMember {
  std::string path;
  std::string bytes;
  mode_t type = AE_IFREG;
};

SkinStorageRoots rootsBelow(const fs::path &root) {
  fs::create_directories(root / "Documents");
  return {.visiblePackages = root / "Documents/Skins",
          .privateRevisions = root / "ApplicationSupport/revisions",
          .privateCatalog = root / "ApplicationSupport/catalog",
          .profileOverlays = root / "ApplicationSupport/overlays"};
}

SkinPackageId packageId() { return *normalizePackageId("FixtureSkin").package; }

void writeBytes(const fs::path &path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<unsigned char> readFile(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string readText(const fs::path &path) {
  const auto bytes = readFile(path);
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

void writeFile(const fs::path &path, std::span<const unsigned char> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

fs::path makeZip(const fs::path &path, const std::vector<ZipMember> &members,
                 bool unsupportedCompression = false) {
  archive *writer = archive_write_new();
  expect(writer != nullptr, "libarchive writer is allocated");
  if (writer == nullptr) {
    return path;
  }
  expect(archive_write_set_format_zip(writer) == ARCHIVE_OK,
         "ZIP format is selected");
  const int compressionStatus =
      unsupportedCompression ? archive_write_zip_set_compression_lzma(writer)
                             : archive_write_zip_set_compression_store(writer);
  expect(compressionStatus == ARCHIVE_OK, "ZIP compression is configured");
  expect(archive_write_open_filename(writer, path.string().c_str()) ==
             ARCHIVE_OK,
         "ZIP output opens");
  for (const ZipMember &member : members) {
    archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, member.path.c_str());
    archive_entry_set_filetype(entry, member.type);
    archive_entry_set_perm(entry, member.type == AE_IFDIR ? 0755 : 0644);
    archive_entry_set_size(entry,
                           member.type == AE_IFREG
                               ? static_cast<la_int64_t>(member.bytes.size())
                               : 0);
    expect(archive_write_header(writer, entry) == ARCHIVE_OK,
           "ZIP member header writes");
    if (member.type == AE_IFREG && !member.bytes.empty()) {
      expect(archive_write_data(writer, member.bytes.data(),
                                member.bytes.size()) ==
                 static_cast<la_ssize_t>(member.bytes.size()),
             "ZIP member data writes");
    }
    expect(archive_write_finish_entry(writer) == ARCHIVE_OK,
           "ZIP member finishes");
    archive_entry_free(entry);
  }
  expect(archive_write_close(writer) == ARCHIVE_OK, "ZIP closes");
  archive_write_free(writer);
  return path;
}

void setEncryptionFlags(const fs::path &path) {
  auto bytes = readFile(path);
  std::size_t localFlags = 0;
  std::size_t centralFlags = 0;
  for (std::size_t index = 0; index + 10 < bytes.size(); ++index) {
    const bool local = bytes[index] == 0x50 && bytes[index + 1] == 0x4b &&
                       bytes[index + 2] == 0x03 && bytes[index + 3] == 0x04;
    const bool central = bytes[index] == 0x50 && bytes[index + 1] == 0x4b &&
                         bytes[index + 2] == 0x01 && bytes[index + 3] == 0x02;
    if (local) {
      bytes[index + 6] |= 0x01;
      ++localFlags;
    } else if (central) {
      bytes[index + 8] |= 0x01;
      ++centralFlags;
    }
  }
  expect(localFlags != 0 && localFlags == centralFlags,
         "encrypted fixture marks matching local and central records");
  writeFile(path, bytes);
}

std::vector<std::uint16_t> centralCompressionMethods(const fs::path &path) {
  const auto bytes = readFile(path);
  std::vector<std::uint16_t> methods;
  for (std::size_t index = 0; index + 12 < bytes.size(); ++index) {
    if (bytes[index] == 0x50 && bytes[index + 1] == 0x4b &&
        bytes[index + 2] == 0x01 && bytes[index + 3] == 0x02) {
      methods.push_back(static_cast<std::uint16_t>(bytes[index + 10]) |
                        (static_cast<std::uint16_t>(bytes[index + 11]) << 8U));
    }
  }
  return methods;
}

std::optional<mode_t> firstCentralUnixType(const fs::path &path) {
  const auto bytes = readFile(path);
  for (std::size_t index = 0; index + 42 < bytes.size(); ++index) {
    if (bytes[index] == 0x50 && bytes[index + 1] == 0x4b &&
        bytes[index + 2] == 0x01 && bytes[index + 3] == 0x02) {
      const std::uint16_t mode =
          static_cast<std::uint16_t>(bytes[index + 40]) |
          (static_cast<std::uint16_t>(bytes[index + 41]) << 8U);
      return static_cast<mode_t>(mode & AE_IFMT);
    }
  }
  return std::nullopt;
}

void patchDeclaredSize(const fs::path &path, std::uint32_t size,
                       bool everyEntry = false) {
  auto bytes = readFile(path);
  bool patched = false;
  for (std::size_t index = 0; index + 28 < bytes.size(); ++index) {
    const bool local = bytes[index] == 0x50 && bytes[index + 1] == 0x4b &&
                       bytes[index + 2] == 0x03 && bytes[index + 3] == 0x04;
    const bool central = bytes[index] == 0x50 && bytes[index + 1] == 0x4b &&
                         bytes[index + 2] == 0x01 && bytes[index + 3] == 0x02;
    if (!local && !central) {
      continue;
    }
    const std::size_t offset = index + (local ? 22 : 24);
    for (int byte = 0; byte < 4; ++byte) {
      bytes[offset + byte] =
          static_cast<unsigned char>((size >> (byte * 8)) & 0xffU);
    }
    patched = true;
    if (!everyEntry && central) {
      break;
    }
  }
  expect(patched, "ZIP declared size fields are patched");
  writeFile(path, bytes);
}

void patchCentralUnixType(const fs::path &path, mode_t type) {
  auto bytes = readFile(path);
  bool patched = false;
  for (std::size_t index = 0; index + 42 < bytes.size(); ++index) {
    const bool central = bytes[index] == 0x50 && bytes[index + 1] == 0x4b &&
                         bytes[index + 2] == 0x01 && bytes[index + 3] == 0x02;
    if (!central) {
      continue;
    }
    bytes[index + 5] = 3;
    const std::uint16_t mode = static_cast<std::uint16_t>(type | 0644);
    bytes[index + 40] = static_cast<unsigned char>(mode & 0xffU);
    bytes[index + 41] = static_cast<unsigned char>((mode >> 8U) & 0xffU);
    patched = true;
    break;
  }
  expect(patched, "ZIP central Unix type is patched");
  writeFile(path, bytes);
  expect(firstCentralUnixType(path) == type,
         "hostile ZIP central metadata has the intended special type");
}

void patchFilenameByte(const fs::path &path, unsigned char from,
                       unsigned char to) {
  auto bytes = readFile(path);
  std::size_t patches = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (bytes[index] == from) {
      bytes[index] = to;
      ++patches;
    }
  }
  expect(patches >= 2, "local and central ZIP names are patched");
  writeFile(path, bytes);
}

void corruptStoredPayload(const fs::path &path, std::string_view payload) {
  auto bytes = readFile(path);
  const auto found =
      std::search(bytes.begin(), bytes.end(), payload.begin(), payload.end());
  expect(found != bytes.end(), "stored ZIP payload is found for corruption");
  if (found != bytes.end()) {
    *found ^= 0x7f;
  }
  writeFile(path, bytes);
}

std::uint16_t readLittle16(const std::vector<unsigned char> &bytes,
                           std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t readLittle32(const std::vector<unsigned char> &bytes,
                           std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void writeLittle32(std::vector<unsigned char> &bytes, std::size_t offset,
                   std::uint32_t value) {
  for (int byte = 0; byte < 4; ++byte) {
    bytes[offset + static_cast<std::size_t>(byte)] =
        static_cast<unsigned char>((value >> (byte * 8)) & 0xffU);
  }
}

std::size_t findEocd(const std::vector<unsigned char> &bytes) {
  for (std::size_t index = bytes.size() - 22;; --index) {
    if (readLittle32(bytes, index) == 0x06054b50U) {
      return index;
    }
    if (index == 0) {
      break;
    }
  }
  throw std::runtime_error("test ZIP has no EOCD");
}

void truncateMemberDataKeepingCentralDirectory(const fs::path &path) {
  auto bytes = readFile(path);
  const std::size_t eocd = findEocd(bytes);
  const std::uint32_t centralOffset = readLittle32(bytes, eocd + 16);
  const std::uint32_t compressedBytes = readLittle32(bytes, centralOffset + 20);
  const std::uint16_t nameBytes = readLittle16(bytes, 26);
  const std::uint16_t extraBytes = readLittle16(bytes, 28);
  const std::size_t dataStart = 30U + nameBytes + extraBytes;
  expect(compressedBytes >= 4 && dataStart + compressedBytes <= centralOffset,
         "member-data truncation fixture has at least four payload bytes");
  bytes.erase(bytes.begin() + dataStart + compressedBytes - 4,
              bytes.begin() + dataStart + compressedBytes);
  writeLittle32(bytes, eocd - 4 + 16, centralOffset - 4);
  writeFile(path, bytes);
}

void truncateCentralRecordKeepingEocd(const fs::path &path) {
  auto bytes = readFile(path);
  const std::size_t eocd = findEocd(bytes);
  expect(eocd >= 3, "central truncation fixture has record bytes");
  bytes.erase(bytes.begin() + eocd - 3, bytes.begin() + eocd);
  writeFile(path, bytes);
}

std::size_t childCount(const fs::path &path) {
  std::error_code error;
  if (!fs::exists(path, error)) {
    return 0;
  }
  return static_cast<std::size_t>(std::distance(
      fs::directory_iterator(path, error), fs::directory_iterator{}));
}

std::size_t publishedRevisionCount(const fs::path &privateRevisions) {
  std::error_code error;
  if (!fs::exists(privateRevisions, error)) {
    return 0;
  }
  std::size_t count = 0;
  for (const fs::directory_entry &entry :
       fs::directory_iterator(privateRevisions, error)) {
    if (entry.path().filename() != ".staging") {
      ++count;
    }
  }
  return count;
}

void expectRejectedAndClean(const PreparePackageResult &result,
                            const SkinStorageRoots &roots,
                            std::string_view behavior) {
  expect(!result.prepared, behavior);
  expect(!result.diagnostics.empty() || result.cancelled,
         "rejection reports a diagnostic or cancellation");
  expect(childCount(roots.privateRevisions / ".staging") == 0,
         "rejection leaves no private revision staging");
  expect(childCount(roots.visiblePackages.parent_path() /
                    ".skin-import-staging") == 0,
         "rejection leaves no visible publication staging");
  expect(publishedRevisionCount(roots.privateRevisions) == 0,
         "rejection publishes no immutable revision");
}

bool hasDiagnosticCode(const PreparePackageResult &result,
                       std::string_view code) {
  return std::ranges::any_of(
      result.diagnostics, [&](const auto &item) { return item.code == code; });
}

PreparePackageResult
prepareZip(const fs::path &zip, const SkinStorageRoots &roots,
           std::stop_token stop = {}, SkinProgressCallback progress = {},
           std::shared_ptr<const SkinImportIoObserver> observer = {}) {
  static NoAliases aliases;
  SkinArchiveImporter importer(roots, aliases, std::move(observer));
  return importer.prepareArchive(zip, packageId(), stop, std::move(progress));
}

PreparePackageResult
prepareFolder(const fs::path &folder, const SkinStorageRoots &roots,
              std::stop_token stop = {}, SkinProgressCallback progress = {},
              std::shared_ptr<const SkinImportIoObserver> observer = {}) {
  static NoAliases aliases;
  SkinArchiveImporter importer(roots, aliases, std::move(observer));
  return importer.prepareFolder(folder, packageId(), stop, std::move(progress));
}

std::vector<std::string> entryPaths(const PreparedPackage &prepared) {
  std::vector<std::string> paths;
  for (const SkinEntryId &entry : prepared.entries()) {
    paths.push_back(entry.packageRelativePath);
  }
  return paths;
}

void testMoveOnlyPreparationContract() {
  static_assert(std::is_move_constructible_v<PreparedPackage>);
  static_assert(std::is_move_assignable_v<PreparedPackage>);
  static_assert(!std::is_copy_constructible_v<PreparedPackage>);
  static_assert(!std::is_copy_assignable_v<PreparedPackage>);
  static_assert(std::is_move_constructible_v<PreparePackageResult>);
  static_assert(!std::is_copy_constructible_v<PreparePackageResult>);
  static_assert(SkinPackagePolicy::maxArchiveBytes ==
                2ULL * 1024 * 1024 * 1024);
  static_assert(SkinPackagePolicy::maxRegularFileBytes == 512ULL * 1024 * 1024);
  static_assert(SkinPackagePolicy::maxExpandedBytes ==
                4ULL * 1024 * 1024 * 1024);
  static_assert(SkinPackagePolicy::maxFiles == 20'000);
  static_assert(SkinPackagePolicy::maxArchiveMembers == 65'535);
  static_assert(SkinPackagePolicy::maxPathBytes == 1'024);
  static_assert(SkinPackagePolicy::maxPathComponents == 64);
}

void testZipFolderAndManualTreeHaveOneIdentity() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path picked = temp.root() / "picked";
  const fs::path fixture = fs::path(ASOBMASHOW_SOURCE_DIR) /
                           "tests/fixtures/beatoraja_skin/packages/minimal";
  fs::copy(fixture, picked, fs::copy_options::recursive);
  writeBytes(picked / "a/first.luaskin", "return {type = 0}\n");
  const fs::path manual = roots.visiblePackages / "FixtureSkin";
  fs::create_directories(manual.parent_path());
  fs::copy(picked, manual, fs::copy_options::recursive);
  const fs::path zip = makeZip(
      temp.root() / "fixture.zip",
      {{"Wrapper/skin/play.luaskin", readText(picked / "skin/play.luaskin")},
       {"Wrapper/skin/module.lua", readText(picked / "skin/module.lua")},
       {"Wrapper/a/first.luaskin", readText(picked / "a/first.luaskin")}});

  auto zipResult = prepareZip(zip, roots);
  auto folderResult = prepareFolder(picked, roots);
  auto manualResult = prepareFolder(manual, roots);
  expect(zipResult.prepared && folderResult.prepared && manualResult.prepared,
         "ZIP, picked folder, and manual direct-child tree all prepare");
  if (zipResult.prepared && folderResult.prepared && manualResult.prepared) {
    const std::string digest =
        zipResult.prepared->candidateRevision().lowercaseSha256;
    expect(folderResult.prepared->candidateRevision().lowercaseSha256 ==
                   digest &&
               manualResult.prepared->candidateRevision().lowercaseSha256 ==
                   digest,
           "ZIP, folder, and manual tree produce one SkinTreeDigestV1");
    expect(entryPaths(*zipResult.prepared) ==
                   (std::vector<std::string>{"a/first.luaskin",
                                             "skin/play.luaskin"}) &&
               entryPaths(*folderResult.prepared) ==
                   entryPaths(*zipResult.prepared) &&
               entryPaths(*manualResult.prepared) ==
                   entryPaths(*zipResult.prepared),
           "all preparation paths recursively discover the same entries");
    expect(
        fs::exists(zipResult.prepared->readView().root() / "skin/module.lua"),
        "package-local module remains in the stable private candidate");
    expect(fs::exists(zipResult.prepared->visibleStagingRoot() /
                      "skin/play.luaskin"),
           "archive extraction owns an unpublished visible staging tree");
  }
}

void testCanonicalWrapperAndExplicitDirectoryRules() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  auto accepted = prepareZip(
      makeZip(temp.root() / "accepted.zip",
              {{"Wrapper/", {}, AE_IFDIR},
               {"Wrapper/skin/", {}, AE_IFDIR},
               {"Wrapper/skin/play.luaskin", "return {type = 0}\n"}}),
      roots);
  expect(accepted.prepared.has_value(),
         "an explicit wrapper root and child directory are structural");
  if (accepted.prepared) {
    expect(entryPaths(*accepted.prepared) ==
               std::vector<std::string>{"skin/play.luaskin"},
           "exactly one common wrapper is stripped");
  }
  accepted.prepared.reset();

  auto rootFile = prepareZip(makeZip(temp.root() / "root.zip",
                                     {{"play.luaskin", "return {type = 0}\n"},
                                      {"module.lua", "return {}\n"}}),
                             roots);
  expect(rootFile.prepared && entryPaths(*rootFile.prepared) ==
                                  std::vector<std::string>{"play.luaskin"},
         "a regular file at archive root forces the no-strip case");
  rootFile.prepared.reset();

  auto multiple =
      prepareZip(makeZip(temp.root() / "multiple.zip",
                         {{"one/play.luaskin", "return {type = 0}\n"},
                          {"two/module.lua", "return {}\n"}}),
                 roots);
  expect(multiple.prepared && entryPaths(*multiple.prepared) ==
                                  std::vector<std::string>{"one/play.luaskin"},
         "multiple regular-file top levels force the no-strip case");
  multiple.prepared.reset();

  auto outsideDirectory =
      prepareZip(makeZip(temp.root() / "outside-directory.zip",
                         {{"outside/", {}, AE_IFDIR},
                          {"Wrapper/play.luaskin", "return {type = 0}\n"}}),
                 roots);
  expectRejectedAndClean(outsideDirectory, roots,
                         "explicit directories outside an inferred wrapper "
                         "reject the whole archive");

  auto wrapperSpecial =
      prepareZip(makeZip(temp.root() / "wrapper-special.zip",
                         {{"Wrapper", {}, AE_IFLNK},
                          {"Wrapper/play.luaskin", "return {type = 0}\n"}}),
                 roots);
  expectRejectedAndClean(wrapperSpecial, roots,
                         "a special entry cannot masquerade as wrapper root");
}

void testUnsafeNamesAndCollisionsRejectWholePackage() {
  struct Case {
    const char *label;
    std::vector<ZipMember> members;
  };
  const std::vector<Case> cases = {
      {"absolute", {{"/play.luaskin", "return {}\n"}}},
      {"drive absolute", {{"C:/play.luaskin", "return {}\n"}}},
      {"traversal", {{"../play.luaskin", "return {}\n"}}},
      {"dot component", {{"./play.luaskin", "return {}\n"}}},
      {"embedded dot", {{"skin/./play.luaskin", "return {}\n"}}},
      {"backslash", {{"skin\\play.luaskin", "return {}\n"}}},
      {"UNC", {{"//server/share/play.luaskin", "return {}\n"}}},
      {"repeated separator", {{"skin//play.luaskin", "return {}\n"}}},
      {"duplicate",
       {{"play.luaskin", "return {}\n"}, {"play.luaskin", "return {}\n"}}},
      {"casefold",
       {{"Stra\xC3\x9F"
         "e/play.luaskin",
         "return {}\n"},
        {"STRASSE/play.luaskin", "return {}\n"}}},
      {"NFC",
       {{"Caf\xC3\xA9/play.luaskin", "return {}\n"},
        {"Cafe\xCC\x81/play.luaskin", "return {}\n"}}},
      {"file directory",
       {{"skin", "regular"},
        {"skin/", {}, AE_IFDIR},
        {"play.luaskin", "return {}\n"}}},
      {"duplicate explicit directory",
       {{"skin/", {}, AE_IFDIR},
        {"skin/", {}, AE_IFDIR},
        {"skin/play.luaskin", "return {}\n"}}},
      {"implicit parent alias",
       {{"Foo/a.luaskin", "return {}\n"}, {"foo/b.lua", "return {}\n"}}},
      {"directory alias collision",
       {{"Caf\xC3\xA9/", {}, AE_IFDIR},
        {"Cafe\xCC\x81/", {}, AE_IFDIR},
        {"play.luaskin", "return {}\n"}}},
      {"wrapper file",
       {{"Wrapper", "file"}, {"Wrapper/play.luaskin", "return {}\n"}}},
      {"post-strip normalization",
       {{"Wrapper/Caf\xC3\xA9.luaskin", "return {}\n"},
        {"Wrapper/Cafe\xCC\x81.luaskin", "return {}\n"}}},
      {"AppleDouble under metadata tree",
       {{"__MACOSX/._play.luaskin", "metadata"},
        {"play.luaskin", "return {}\n"}}},
      {"AppleDouble at any level",
       {{"skin/._module.lua", "metadata"},
        {"skin/play.luaskin", "return {}\n"}}},
  };
  for (std::size_t index = 0; index < cases.size(); ++index) {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    auto result =
        prepareZip(makeZip(temp.root() / (std::to_string(index) + ".zip"),
                           cases[index].members),
                   roots);
    expectRejectedAndClean(result, roots, cases[index].label);
  }
}

void testInvalidUtf8AndNulNamesReject() {
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const std::string invalid =
        std::string("bad") + static_cast<char>(0xff) + "/play.luaskin";
    auto result = prepareZip(
        makeZip(temp.root() / "invalid-utf8.zip", {{invalid, "return {}\n"}}),
        roots);
    expectRejectedAndClean(result, roots, "invalid UTF-8 archive names reject");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "nul.zip",
                                 {{"badXname/play.luaskin", "return {}\n"}});
    patchFilenameByte(zip, 'X', 0);
    expectRejectedAndClean(prepareZip(zip, roots), roots,
                           "embedded NUL archive names reject");
  }
}

void testLinksAndNonregularEntriesRejectWholePackage() {
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    auto result = prepareZip(
        makeZip(temp.root() / "symlink.zip",
                {{"play.luaskin", "return {}\n"}, {"link", {}, AE_IFLNK}}),
        roots);
    expectRejectedAndClean(result, roots, "symbolic links reject");
  }
  const std::vector<std::pair<std::string, mode_t>> patchedTypes = {
      {"FIFO", AE_IFIFO},
      {"socket", AE_IFSOCK},
      {"character device", AE_IFCHR},
  };
  for (std::size_t index = 0; index < patchedTypes.size(); ++index) {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip =
        makeZip(temp.root() / (std::to_string(index) + ".zip"),
                {{"special", ""}, {"play.luaskin", "return {}\n"}});
    patchCentralUnixType(zip, patchedTypes[index].second);
    auto result = prepareZip(zip, roots);
    expectRejectedAndClean(result, roots, patchedTypes[index].first);
  }
}

void testEncryptedTruncatedCrcAndUnsupportedCompressionReject() {
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "encrypted.zip",
                                 {{"play.luaskin", "return {type = 0}\n"}});
    setEncryptionFlags(zip);
    auto result = prepareZip(zip, roots);
    expectRejectedAndClean(result, roots,
                           "encrypted members reject the archive");
    expect(hasDiagnosticCode(result, "skin_archive_member_metadata_invalid"),
           "encrypted member rejection has a stable diagnostic code");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "truncated.zip",
                                 {{"play.luaskin", "return {type = 0}\n"}});
    fs::resize_file(zip, fs::file_size(zip) - 8);
    auto result = prepareZip(zip, roots);
    expectRejectedAndClean(result, roots,
                           "truncated central directory rejects the archive");
    expect(hasDiagnosticCode(result, "skin_archive_eocd_invalid"),
           "EOCD truncation has a stable diagnostic code");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip =
        makeZip(temp.root() / "member-data-truncated.zip",
                {{"play.luaskin", "return {type = 123456789}\n"}});
    truncateMemberDataKeepingCentralDirectory(zip);
    auto result = prepareZip(zip, roots);
    expectRejectedAndClean(result, roots,
                           "truncated member data rejects the archive");
    expect(hasDiagnosticCode(result, "skin_archive_crc_or_read_failed"),
           "member-data truncation has a stable diagnostic code");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "central-record-truncated.zip",
                                 {{"play.luaskin", "return {}\n"}});
    truncateCentralRecordKeepingEocd(zip);
    auto result = prepareZip(zip, roots);
    expectRejectedAndClean(result, roots,
                           "truncated central record rejects the archive");
    expect(hasDiagnosticCode(result, "skin_archive_directory_invalid"),
           "central-record truncation has a stable diagnostic code");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const std::string payload = "return {type = 123456789}\n";
    const fs::path zip =
        makeZip(temp.root() / "crc.zip", {{"play.luaskin", payload}});
    corruptStoredPayload(zip, payload);
    auto result = prepareZip(zip, roots);
    expectRejectedAndClean(result, roots, "CRC mismatch rejects the archive");
    expect(hasDiagnosticCode(result, "skin_archive_crc_or_read_failed"),
           "CRC rejection has a stable diagnostic code");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip =
        makeZip(temp.root() / "unsupported.zip",
                {{"play.luaskin", "return {type = 0}\n"}}, true);
    expect(centralCompressionMethods(zip) == std::vector<std::uint16_t>{14},
           "unsupported-compression fixture really uses ZIP LZMA");
    auto result = prepareZip(zip, roots);
    expectRejectedAndClean(result, roots,
                           "unsupported ZIP compression rejects the archive");
    expect(hasDiagnosticCode(result, "skin_archive_member_metadata_invalid"),
           "unsupported compression has a stable diagnostic code");
  }
}

void testProgressCallbackFailureRejectsAndCleans() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path zip = makeZip(temp.root() / "callback.zip",
                               {{"play.luaskin", "return {type = 0}\n"}});
  auto result = prepareZip(zip, roots, {}, [](const SkinProgress &) {
    throw std::runtime_error("test callback failure");
  });
  expectRejectedAndClean(result, roots,
                         "a throwing progress callback fails closed");
}

void testArchivePayloadDigestMustMatchTask5Snapshot() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path zip =
      makeZip(temp.root() / "payload-race.zip",
              {{"play.luaskin", "return {marker = 'archive'}\n"}});
  auto observer = std::make_shared<ImportIoObserver>(
      [&](SkinImportIoOperation operation, const fs::path &path) {
        if (operation == SkinImportIoOperation::BeforeVisibleSnapshot) {
          writeBytes(path / "play.luaskin", "return {marker = 'changed'}\n");
        }
      });
  auto result = prepareZip(zip, roots, {}, {}, observer);
  expectRejectedAndClean(
      result, roots,
      "Task5 candidate must match the streamed archive payload digest");
  expect(hasDiagnosticCode(result, "skin_archive_payload_digest_mismatch"),
         "archive payload mismatch has a stable diagnostic code");
}

void testStreamedPayloadDigestRejectsLastChunkStagingMutation() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const std::string archived = "return {marker = 'archive'}\n";
  const std::string replaced = "return {marker = 'changed'}\n";
  expect(archived.size() == replaced.size(),
         "last-chunk mutation fixture preserves the declared file size");
  const fs::path zip = makeZip(temp.root() / "streamed-payload-race.zip",
                               {{"play.luaskin", archived}});
  bool mutated = false;
  auto observer = std::make_shared<ImportIoObserver>(
      [&](SkinImportIoOperation operation, const fs::path &path) {
        if (operation == SkinImportIoOperation::AfterVisibleFileWritten &&
            path.filename() == "play.luaskin") {
          writeBytes(path, replaced);
          mutated = readText(path) == replaced;
        }
      });
  auto result = prepareZip(zip, roots, {}, {}, observer);
  expect(mutated, "last-chunk mutation runs after the streamed archive write");
  expectRejectedAndClean(
      result, roots,
      "staged bytes must remain cryptographically bound to streamed ZIP bytes");
  expect(hasDiagnosticCode(result, "skin_archive_payload_digest_mismatch"),
         "streamed member mismatch has a stable diagnostic code");
}

void testStagedFifoRaceRejectsWithoutBlockingOpen() {
#if !defined(_WIN32)
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  std::atomic_bool writerAttempted{false};
  std::optional<std::jthread> delayedWriter;
  bool fifoInstalled = false;
  auto observer = std::make_shared<ImportIoObserver>(
      [&](SkinImportIoOperation operation, const fs::path &path) {
        if (operation != SkinImportIoOperation::AfterVisibleFileWritten ||
            path.filename() != "play.luaskin") {
          return;
        }
        fs::remove(path);
        fifoInstalled = ::mkfifo(path.c_str(), 0600) == 0;
        delayedWriter.emplace([path, &writerAttempted] {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          writerAttempted.store(true);
          const int writer =
              ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
          if (writer >= 0) {
            ::close(writer);
          }
        });
      });
  auto result = prepareZip(makeZip(temp.root() / "staged-fifo-race.zip",
                                   {{"play.luaskin", "return {}\n"}}),
                           roots, {}, {}, observer);
  const bool returnedBeforeWriter = !writerAttempted.load();
  delayedWriter.reset();
  expect(fifoInstalled, "staged FIFO race fixture replaced the regular file");
  expect(returnedBeforeWriter,
         "secure staged-file reopen rejects a FIFO without waiting for a "
         "writer");
  expectRejectedAndClean(result, roots,
                         "a raced staged FIFO rejects package preparation");
  expect(hasDiagnosticCode(result, "skin_archive_payload_digest_failed"),
         "staged FIFO rejection has a stable diagnostic code");
#endif
}

void testVisibleStagingRejectsParentAndLeafSymlinkRaces() {
#if !defined(_WIN32)
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path victim = temp.root() / "parent-race-victim";
    writeBytes(victim / "sentinel", "unchanged");
    fs::path displaced;
    auto observer = std::make_shared<ImportIoObserver>(
        [&](SkinImportIoOperation operation, const fs::path &path) {
          if (operation != SkinImportIoOperation::VisibleRootIssued) {
            return;
          }
          displaced = path.parent_path() / "displaced-issued-root";
          fs::rename(path, displaced);
          fs::create_directory_symlink(victim, path);
        });
    auto result = prepareZip(makeZip(temp.root() / "parent-race.zip",
                                     {{"play.luaskin", "return {}\n"}}),
                             roots, {}, {}, observer);
    expect(!result.prepared,
           "replacing the issued staging root rejects the archive");
    expect(hasDiagnosticCode(result, "skin_import_staging_identity_changed"),
           "staging-root replacement has a stable diagnostic code");
    expect(readText(victim / "sentinel") == "unchanged" &&
               !fs::exists(victim / "play.luaskin"),
           "staging-root symlink replacement cannot write into its target");
    expect(!displaced.empty() && !fs::exists(displaced),
           "cleanup finds and removes only the displaced issued root identity");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path folder = temp.root() / "picked";
    const fs::path victim = temp.root() / "leaf-race-victim";
    writeBytes(folder / "play.luaskin", "return {}\n");
    writeBytes(victim, "unchanged");
    auto observer = std::make_shared<ImportIoObserver>(
        [&](SkinImportIoOperation operation, const fs::path &path) {
          if (operation == SkinImportIoOperation::BeforeVisibleFile) {
            fs::create_symlink(victim, path);
          }
        });
    auto result = prepareFolder(folder, roots, {}, {}, observer);
    expectRejectedAndClean(result, roots,
                           "a symlink raced into a visible file leaf rejects");
    expect(hasDiagnosticCode(result, "skin_import_visible_copy_failed"),
           "visible leaf race has a stable diagnostic code");
    expect(readText(victim) == "unchanged",
           "exclusive no-follow leaf creation cannot truncate a symlink "
           "target");
  }
#endif
}

void testDisplacedNestedParentCleanupRemovesImportedBytes() {
#if !defined(_WIN32)
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path displaced = temp.root() / "displaced-open-parent";
    bool displacedBeforeOpen = false;
    auto observer = std::make_shared<ImportIoObserver>(
        [&](SkinImportIoOperation operation, const fs::path &path) {
          if (!displacedBeforeOpen &&
              operation == SkinImportIoOperation::BeforeVisibleFile &&
              path.filename() == "nested.bin") {
            fs::rename(path.parent_path(), displaced);
            displacedBeforeOpen = true;
          }
        });
    auto result = prepareZip(makeZip(temp.root() / "open-parent-race.zip",
                                     {{"play.luaskin", "return {}\n"},
                                      {"assets/nested.bin", "payload"}}),
                             roots, {}, {}, observer);
    expect(displacedBeforeOpen,
           "nested parent is displaced between parent open and leaf open");
    expectRejectedAndClean(result, roots,
                           "pre-write nested-parent displacement rejects");
    expect(!fs::exists(displaced / "nested.bin"),
           "pre-write identity check prevents bytes escaping through the "
           "opened parent capability");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const std::string entry = "return {type = 0}\n";
    const std::string asset(96 * 1024, 'a');
    const fs::path zip =
        makeZip(temp.root() / "nested-parent-race.zip",
                {{"play.luaskin", entry}, {"assets/nested.bin", asset}});
    fs::path issuedRoot;
    const fs::path displaced = temp.root() / "displaced-nested-parent";
    auto observer = std::make_shared<ImportIoObserver>(
        [&](SkinImportIoOperation operation, const fs::path &path) {
          if (operation == SkinImportIoOperation::VisibleRootIssued) {
            issuedRoot = path;
          }
        });
    bool displacedAfterWrite = false;
    auto result = prepareZip(
        zip, roots, {},
        [&](const SkinProgress &progress) {
          if (!displacedAfterWrite && !issuedRoot.empty() &&
              progress.phase == SkinProgressPhase::Copying &&
              progress.totalBytes != 0 &&
              progress.completedBytes == progress.totalBytes) {
            fs::rename(issuedRoot / "assets", displaced);
            displacedAfterWrite = true;
          }
        },
        observer);
    expect(displacedAfterWrite,
           "nested staging parent is displaced after its final write");
    expectRejectedAndClean(result, roots,
                           "nested-parent displacement rejects preparation");
    expect(!fs::exists(displaced / "nested.bin"),
           "identity-bound cleanup removes imported bytes from a displaced "
           "nested parent");
  }
#endif
}

#if defined(_WIN32)
bool hasProtectedCurrentUserOnlyDacl(const fs::path &path) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD result = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
      &dacl, nullptr, &descriptor);
  HANDLE token = nullptr;
  DWORD tokenBytes = 0;
  std::vector<std::byte> tokenUser;
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  void *rawAce = nullptr;
  bool privateAcl =
      result == ERROR_SUCCESS && descriptor != nullptr && owner != nullptr &&
      dacl != nullptr && dacl->AceCount == 1 &&
      GetSecurityDescriptorControl(descriptor, &control, &revision) &&
      (control & SE_DACL_PROTECTED) != 0 && GetAce(dacl, 0, &rawAce) &&
      rawAce != nullptr &&
      static_cast<ACE_HEADER *>(rawAce)->AceType == ACCESS_ALLOWED_ACE_TYPE &&
      OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
  if (privateAcl) {
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
    tokenUser.resize(tokenBytes);
    privateAcl = tokenBytes > 0 &&
                 GetTokenInformation(token, TokenUser, tokenUser.data(),
                                     tokenBytes, &tokenBytes);
  }
  if (privateAcl) {
    const auto *user = reinterpret_cast<const TOKEN_USER *>(tokenUser.data());
    const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(rawAce);
    privateAcl =
        EqualSid(owner, user->User.Sid) &&
        EqualSid(const_cast<DWORD *>(&ace->SidStart), user->User.Sid) &&
        ace->Header.AceFlags == 0 && ace->Mask == FILE_ALL_ACCESS;
  }
  if (token != nullptr) {
    CloseHandle(token);
  }
  if (descriptor != nullptr) {
    LocalFree(descriptor);
  }
  return privateAcl;
}
#endif

void testWindowsVisibleStagingUsesProtectedOwnerOnlyDacls() {
#if defined(_WIN32)
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  auto result = prepareZip(
      makeZip(temp.root() / "private-dacl.zip",
              {{"play.luaskin", "return {}\n"}, {"assets/data.bin", "x"}}),
      roots);
  expect(result.prepared.has_value(),
         "Windows DACL fixture prepares for permission inspection");
  if (!result.prepared) {
    return;
  }
  const fs::path staging = result.prepared->visibleStagingRoot();
  expect(hasProtectedCurrentUserOnlyDacl(staging.parent_path()),
         "shared staging parent has a protected current-user-only DACL");
  expect(hasProtectedCurrentUserOnlyDacl(staging),
         "issued staging root has a protected current-user-only DACL");
  expect(hasProtectedCurrentUserOnlyDacl(staging / "assets"),
         "nested staging directory has a protected current-user-only DACL");
  expect(hasProtectedCurrentUserOnlyDacl(staging / "assets/data.bin"),
         "staged file has a protected current-user-only DACL");
#endif
}

void testEmptyAndDirectoryOnlyArchivesReject() {
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    expectRejectedAndClean(
        prepareZip(makeZip(temp.root() / "empty.zip", {}), roots), roots,
        "empty ZIPs reject");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    expectRejectedAndClean(
        prepareZip(makeZip(temp.root() / "dirs.zip", {{"skin/", {}, AE_IFDIR}}),
                   roots),
        roots, "directory-only ZIPs reject");
  }
}

void testZipDeclaredAndAggregateLimitsRejectBeforeExtraction() {
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "per-file.zip",
                                 {{"play.luaskin", "return {type = 0}\n"}});
    patchDeclaredSize(zip, static_cast<std::uint32_t>(
                               SkinPackagePolicy::maxRegularFileBytes + 1));
    expectRejectedAndClean(prepareZip(zip, roots), roots,
                           "ZIP declared files over the per-file cap reject");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    std::vector<ZipMember> members;
    for (int index = 0; index < 9; ++index) {
      members.push_back(
          {index == 0 ? "play.luaskin" : "f" + std::to_string(index), "x"});
    }
    const fs::path zip = makeZip(temp.root() / "aggregate.zip", members);
    patchDeclaredSize(
        zip, static_cast<std::uint32_t>(SkinPackagePolicy::maxRegularFileBytes),
        true);
    expectRejectedAndClean(prepareZip(zip, roots), roots,
                           "ZIP aggregate declarations over 4 GiB reject");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "declared-actual.zip",
                                 {{"play.luaskin", "return {type = 0}\n"}});
    patchDeclaredSize(zip, 64);
    expectRejectedAndClean(prepareZip(zip, roots), roots,
                           "declared-vs-actual size mismatch rejects");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    std::vector<ZipMember> members;
    members.reserve(SkinPackagePolicy::maxFiles + 1);
    for (std::uint64_t index = 0; index < SkinPackagePolicy::maxFiles;
         ++index) {
      members.push_back({"d" + std::to_string(index) + "/", {}, AE_IFDIR});
    }
    members.push_back({"play.luaskin", "return {}\n"});
    auto result = prepareZip(
        makeZip(temp.root() / "directory-count.zip", members), roots);
    expect(result.prepared.has_value(),
           "maxFiles counts regular files, not explicit directories");
  }
}

void testFixedPathDepthCountAndArchiveLimitsReject() {
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path folder = temp.root() / "per-file";
    fs::create_directories(folder);
    writeBytes(folder / "too-large.luaskin", {});
    fs::resize_file(folder / "too-large.luaskin",
                    SkinPackagePolicy::maxRegularFileBytes + 1);
    expectRejectedAndClean(prepareFolder(folder, roots), roots,
                           "regular files over the fixed byte limit reject");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path folder = temp.root() / "expanded";
    fs::create_directories(folder);
    for (int index = 0; index < 9; ++index) {
      writeBytes(folder / ("large-" + std::to_string(index)), {});
      fs::resize_file(folder / ("large-" + std::to_string(index)),
                      SkinPackagePolicy::maxRegularFileBytes);
    }
    expectRejectedAndClean(prepareFolder(folder, roots), roots,
                           "expanded trees over the fixed byte limit reject");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    std::string path(SkinPackagePolicy::maxPathBytes + 1, 'a');
    path += ".luaskin";
    expectRejectedAndClean(
        prepareZip(makeZip(temp.root() / "path.zip", {{path, "x"}}), roots),
        roots, "normalized paths over the fixed byte limit reject");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    std::string path;
    for (std::uint32_t component = 0;
         component < SkinPackagePolicy::maxPathComponents + 1; ++component) {
      path += "a/";
    }
    path += "play.luaskin";
    expectRejectedAndClean(
        prepareZip(makeZip(temp.root() / "depth.zip", {{path, "x"}}), roots),
        roots, "paths over the fixed component limit reject");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    std::vector<ZipMember> members;
    members.reserve(SkinPackagePolicy::maxFiles + 1);
    for (std::uint64_t index = 0; index < SkinPackagePolicy::maxFiles + 1;
         ++index) {
      members.push_back({"f" + std::to_string(index), {}});
    }
    members.front().path = "play.luaskin";
    expectRejectedAndClean(
        prepareZip(makeZip(temp.root() / "count.zip", members), roots), roots,
        "archives over the fixed regular-file count reject");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "oversize.zip",
                                 {{"play.luaskin", "return {}\n"}});
    fs::resize_file(zip, SkinPackagePolicy::maxArchiveBytes + 1);
    expectRejectedAndClean(prepareZip(zip, roots), roots,
                           "archive files over the fixed byte limit reject");
  }
}

void testArchiveMutationAndMidOperationCancellationRejectCleanly() {
#if !defined(_WIN32)
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip =
        makeZip(temp.root() / "same-inode-change.zip",
                {{"play.luaskin", std::string(256 * 1024, 'x')}});
    bool mutated = false;
    auto observer = std::make_shared<ImportIoObserver>(
        [&](SkinImportIoOperation operation, const fs::path &) {
          if (!mutated &&
              operation == SkinImportIoOperation::OwnedArchiveCopyChunk) {
            std::fstream stream(zip, std::ios::binary | std::ios::in |
                                         std::ios::out);
            stream.seekp(100);
            stream.put('y');
            stream.flush();
            mutated = true;
          }
        });
    auto result = prepareZip(zip, roots, {}, {}, observer);
    expect(mutated, "same-inode source mutation fixture executed");
    expectRejectedAndClean(result, roots,
                           "same-inode mutation during owned copy rejects");
    expect(hasDiagnosticCode(result, "skin_archive_source_changed"),
           "same-inode source mutation is detected at the copy boundary");
  }
#endif
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip =
        makeZip(temp.root() / "same-manifest.zip",
                {{"play.luaskin", "return {type = 0, marker = 'A'}\n"}});
    const fs::path replacement =
        makeZip(temp.root() / "replacement.zip",
                {{"play.luaskin", "return {type = 0, marker = 'B'}\n"}});
    const fs::path original = temp.root() / "original.zip";
    const fs::path extractedReplacement = temp.root() / "used-replacement.zip";
    int inspectingReports = 0;
    bool restored = false;
    auto result = prepareZip(zip, roots, {}, [&](const SkinProgress &progress) {
      if (progress.phase == SkinProgressPhase::Inspecting &&
          ++inspectingReports == 2) {
        fs::rename(zip, original);
        fs::rename(replacement, zip);
      } else if (!restored && progress.phase == SkinProgressPhase::Copying &&
                 progress.completedBytes != 0) {
        fs::rename(zip, extractedReplacement);
        fs::rename(original, zip);
        restored = true;
      }
    });
    expect(restored, "same-manifest source-swap fixture restores original ZIP");
    expect(result.prepared.has_value(),
           "source pathname replacement does not invalidate the owned ZIP");
    if (result.prepared) {
      expect(readText(result.prepared->readView().root() / "play.luaskin") ==
                 "return {type = 0, marker = 'A'}\n",
             "inventory and payload bytes come from the one initially opened "
             "archive object");
    }
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "changed.zip",
                                 {{"play.luaskin", "return {type = 0}\n"}});
    int inspectingReports = 0;
    auto result = prepareZip(zip, roots, {}, [&](const SkinProgress &progress) {
      if (progress.phase == SkinProgressPhase::Inspecting &&
          ++inspectingReports == 2) {
        makeZip(zip, {{"other.luaskin", "return {type = 1}\n"}});
      }
    });
    expect(result.prepared.has_value(),
           "archive pathname replacement cannot change the owned ZIP");
    if (result.prepared) {
      expect(readText(result.prepared->readView().root() / "play.luaskin") ==
                 "return {type = 0}\n",
             "the originally opened archive remains authoritative");
    }
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "header-cancel.zip",
                                 {{"a", "x"}, {"play.luaskin", "return {}\n"}});
    std::stop_source source;
    auto result =
        prepareZip(zip, roots, source.get_token(),
                   [&source](const SkinProgress &progress) {
                     if (progress.phase == SkinProgressPhase::Inspecting) {
                       source.request_stop();
                     }
                   });
    expect(result.cancelled, "cancellation between ZIP headers is reported");
    expectRejectedAndClean(result, roots,
                           "mid-header cancellation leaves no staging");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip =
        makeZip(temp.root() / "data-cancel.zip",
                {{"play.luaskin", std::string(256 * 1024, 'x')}});
    std::stop_source source;
    auto result =
        prepareZip(zip, roots, source.get_token(),
                   [&source](const SkinProgress &progress) {
                     if (progress.phase == SkinProgressPhase::Copying &&
                         progress.completedBytes != 0) {
                       source.request_stop();
                     }
                   });
    expect(result.cancelled,
           "cancellation during streamed ZIP data is reported");
    expectRejectedAndClean(result, roots,
                           "mid-data cancellation leaves no staging");
  }
}

void testCancellationCoversOwnedCopyRawScanAndHash() {
  struct Case {
    SkinImportIoOperation operation;
    const char *label;
  };
  const std::vector<Case> cases = {
      {SkinImportIoOperation::OwnedArchiveCopyChunk,
       "cancellation during owned archive copy"},
      {SkinImportIoOperation::RawZipRecord,
       "cancellation during raw ZIP central scan"},
      {SkinImportIoOperation::OwnedArchiveHashChunk,
       "cancellation during owned archive hashing"},
  };
  for (std::size_t index = 0; index < cases.size(); ++index) {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    std::vector<ZipMember> members = {
        {"play.luaskin", std::string(256 * 1024, 'x')}};
    for (int extra = 0; extra < 8; ++extra) {
      members.push_back({"extra-" + std::to_string(extra), "x"});
    }
    const fs::path zip = makeZip(
        temp.root() / ("cancel-phase-" + std::to_string(index) + ".zip"),
        members);
    std::stop_source source;
    auto observer = std::make_shared<ImportIoObserver>(
        [&](SkinImportIoOperation operation, const fs::path &) {
          if (operation == cases[index].operation) {
            source.request_stop();
          }
        });
    auto result = prepareZip(zip, roots, source.get_token(), {}, observer);
    expect(result.cancelled, cases[index].label);
    expect(result.diagnostics.empty(),
           "cooperative pre-extraction cancellation is not an error");
    expectRejectedAndClean(result, roots, cases[index].label);
  }
}

void testVisibleSkinsRootIsNotWidenedIntoOnePackage() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  writeBytes(roots.visiblePackages / "loose.luaskin", "return {}\n");
  writeBytes(roots.visiblePackages / "DirectChild/z.luaskin", "return {}\n");
  writeBytes(roots.visiblePackages / "DirectChild/a/first.luaskin",
             "return {}\n");
  expectRejectedAndClean(prepareFolder(roots.visiblePackages, roots), roots,
                         "canonical Skins root and loose files are not one "
                         "package");
  auto direct = prepareFolder(roots.visiblePackages / "DirectChild", roots);
  expect(direct.prepared.has_value(),
         "a direct-child manual package tree is accepted");
  if (direct.prepared) {
    expect(entryPaths(*direct.prepared) ==
               (std::vector<std::string>{"a/first.luaskin", "z.luaskin"}),
           "recursive manual entries use canonical byte sorting");
  }
}

void testVisibleStagingPreservesExistingFileProviderPermissions() {
#if defined(_WIN32)
  return;
#else
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path stagingParent =
      roots.visiblePackages.parent_path() / ".skin-import-staging";
  fs::create_directories(stagingParent);
  expect(::chmod(stagingParent.c_str(), 0755) == 0,
         "fixture makes Documents-visible staging provider-readable");

  const auto result = prepareZip(
      makeZip(temp.root() / "provider-permissions.zip",
              {{"play.luaskin", "return {type = 0}\\n"}}),
      roots);
  struct stat status {};
  expect(result.prepared.has_value() &&
             ::stat(stagingParent.c_str(), &status) == 0 &&
             (status.st_mode & 0777) == 0755,
         "visible staging does not require or overwrite File Provider permissions");
#endif
}

void testPreparedPackageMovesOwnCleanupAndReadViewLifetime() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path firstZip = makeZip(temp.root() / "first.zip",
                                    {{"play.luaskin", "return {type = 0}\n"}});
  const fs::path secondZip = makeZip(temp.root() / "second.zip",
                                     {{"play.luaskin", "return {type = 1}\n"}});
  auto firstResult = prepareZip(firstZip, roots);
  auto secondResult = prepareZip(secondZip, roots);
  expect(firstResult.prepared && secondResult.prepared,
         "two packages prepare for runtime move ownership checks");
  if (!firstResult.prepared || !secondResult.prepared) {
    return;
  }
  const fs::path firstRoot = firstResult.prepared->readView().root();
  const std::string secondDigest =
      secondResult.prepared->readView().revision().lowercaseSha256;
  PreparedPackage owner(std::move(*firstResult.prepared));
  firstResult.prepared.reset();
  expect(fs::exists(firstRoot / "play.luaskin"),
         "move construction preserves read-view candidate lifetime");
  owner = std::move(*secondResult.prepared);
  secondResult.prepared.reset();
  expect(!fs::exists(firstRoot),
         "move assignment cleans the previously owned candidate");
  expect(owner.readView().revision().lowercaseSha256 == secondDigest &&
             fs::exists(owner.readView().root() / "play.luaskin"),
         "move assignment transfers candidate and read-view lifetime");
}

void testFolderAppleDoubleRejectsWithArchiveParity() {
  TempDirectory temp;
  const auto roots = rootsBelow(temp.root());
  const fs::path folder = temp.root() / "picked";
  writeBytes(folder / "play.luaskin", "return {type = 0}\n");
  writeBytes(folder / "assets/._image.png", "metadata");
  expectRejectedAndClean(prepareFolder(folder, roots), roots,
                         "folder AppleDouble sidecars reject like ZIPs");
}

void testCancellationAndPreparedDestructionCleanAllStaging() {
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "cancelled.zip",
                                 {{"play.luaskin", "return {type = 0}\n"}});
    std::stop_source source;
    source.request_stop();
    auto result = prepareZip(zip, roots, source.get_token());
    expect(result.cancelled, "pre-requested archive cancellation is reported");
    expectRejectedAndClean(result, roots,
                           "cancelled archive preparation publishes nothing");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path folder = temp.root() / "picked";
    writeBytes(folder / "play.luaskin", "return {type = 0}\n");
    std::stop_source source;
    auto result = prepareFolder(
        folder, roots, source.get_token(),
        [&source](const SkinProgress &) { source.request_stop(); });
    expect(result.cancelled,
           "folder cancellation during Task5 snapshot is reported");
    expectRejectedAndClean(result, roots,
                           "cancelled folder preparation publishes nothing");
  }
  {
    TempDirectory temp;
    const auto roots = rootsBelow(temp.root());
    const fs::path zip = makeZip(temp.root() / "owned.zip",
                                 {{"play.luaskin", "return {type = 0}\n"}});
    {
      auto result = prepareZip(zip, roots);
      expect(result.prepared.has_value(), "valid archive prepares");
      expect(childCount(roots.privateRevisions / ".staging") == 1,
             "prepared package owns private revision staging");
      expect(childCount(roots.visiblePackages.parent_path() /
                        ".skin-import-staging") == 1,
             "prepared package owns visible staging");
    }
    expect(childCount(roots.privateRevisions / ".staging") == 0,
           "PreparedPackage destruction cleans private revision staging");
    expect(childCount(roots.visiblePackages.parent_path() /
                      ".skin-import-staging") == 0,
           "PreparedPackage destruction cleans visible staging");
  }
}

} // namespace

int main() {
  testMoveOnlyPreparationContract();
  testZipFolderAndManualTreeHaveOneIdentity();
  testCanonicalWrapperAndExplicitDirectoryRules();
  testUnsafeNamesAndCollisionsRejectWholePackage();
  testInvalidUtf8AndNulNamesReject();
  testLinksAndNonregularEntriesRejectWholePackage();
  testEncryptedTruncatedCrcAndUnsupportedCompressionReject();
  testProgressCallbackFailureRejectsAndCleans();
  testArchivePayloadDigestMustMatchTask5Snapshot();
  testStreamedPayloadDigestRejectsLastChunkStagingMutation();
  testStagedFifoRaceRejectsWithoutBlockingOpen();
  testVisibleStagingRejectsParentAndLeafSymlinkRaces();
  testDisplacedNestedParentCleanupRemovesImportedBytes();
  testWindowsVisibleStagingUsesProtectedOwnerOnlyDacls();
  testEmptyAndDirectoryOnlyArchivesReject();
  testZipDeclaredAndAggregateLimitsRejectBeforeExtraction();
  testFixedPathDepthCountAndArchiveLimitsReject();
  testFolderAppleDoubleRejectsWithArchiveParity();
  testArchiveMutationAndMidOperationCancellationRejectCleanly();
  testCancellationCoversOwnedCopyRawScanAndHash();
  testVisibleSkinsRootIsNotWidenedIntoOnePackage();
  testVisibleStagingPreservesExistingFileProviderPermissions();
  testPreparedPackageMovesOwnCleanupAndReadViewLifetime();
  testCancellationAndPreparedDestructionCleanAllStaging();
  if (failures != 0) {
    std::cerr << failures << " importer assertion(s) failed\n";
    return 1;
  }
  std::cout << "skin archive importer tests passed\n";
  return 0;
}
