#include "../src/storage/StorageMeasurement.h"
#include "../src/storage/StorageArchiveMeasurement.h"
#include "../src/ArchiveFile.h"
#include "../src/ArchiveRAII.h"
#include "StorageTestSupport.h"
#include "fixtures/archive/mixed_encryption_zip.h"
#include <archive_entry.h>
#include <atomic>
#include <iostream>
#include <limits>

using namespace storage_management;
using storage_test::require;
using storage_test::TempDirectory;
namespace fs = std::filesystem;
namespace {
void writeArchive(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &files,
                  bool solid = false) {
  auto archive = makeArchiveWriteHandle();
  require((solid ? archive_write_set_format_7zip(archive.get()) : archive_write_set_format_zip(archive.get())) == ARCHIVE_OK,
          "set fixture archive format");
  require(archive_write_open_filename(archive.get(), path.string().c_str()) == ARCHIVE_OK, "open fixture archive");
  for (const auto &[name, data] : files) {
    auto *entry = archive_entry_new();
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, data.size());
    require(archive_write_header(archive.get(), entry) == ARCHIVE_OK, "write fixture header");
    require(archive_write_data(archive.get(), data.data(), data.size()) == static_cast<la_ssize_t>(data.size()), "write fixture data");
    archive_entry_free(entry);
  }
  require(archive_write_close(archive.get()) == ARCHIVE_OK, "close fixture archive");
}
FolderSelection archiveGroup(std::string id, fs::path path) {
  return {.groupId = std::move(id), .path = std::move(path), .writableLocal = false, .sourceKind = StorageSourceKind::Archive};
}
void sizeOnlyDoesNotBuildPreflight() {
  TempDirectory temp;
  temp.write("a.wav", "12345"); temp.write("sub/b.png", "123");
  fs::create_hard_link(temp.root / "a.wav", temp.root / "hard.wav");
  fs::create_directory_symlink(temp.root / "sub", temp.root / "link");
  InventoryRequest request;
  request.groups = {{"root", temp.root}, {"sub", temp.root / "sub"}, {"duplicate", temp.root}};
  request.policy.audio = AudioAction::LossyVorbis;
  StorageScanDependencies deps;
  std::atomic<unsigned> visits{0};
  deps.enumerate = [&](const auto &path, const auto &root, auto mode, auto check, auto visitor,
                       auto &stamp, auto *metrics, auto &error) {
    require(mode == StorageVisitMode::SizeOnly, "browsing never asks for mutation preflight");
    ++visits;
    return VisitStorageDirectory(path, root, mode, check, [&](const auto &entry) {
      require(!entry.identity, "browsing retains no file identity");
      return visitor(entry);
    }, stamp, metrics, error);
  };
  StorageScanMetrics metrics;
  std::size_t updates = 0;
  const auto measured = MeasureStorageGroups(request, {}, [&](const auto &batch) { updates += batch.size(); }, &metrics, deps);
  require(measured.complete && measured.groups.size() == 3 && updates == 3, "publish each completed group");
  require(measured.totalBytes == 13 && measured.groups[0].totalBytes == 13 && measured.groups[1].totalBytes == 3 &&
              measured.groups[2].audioBytes == 10, "duplicate/nested groups preserve hard-link logical bytes and union");
  require(visits == 2 && metrics.metadataReads == 5 && measured.directories.size() == 2,
          "enumerate each physical directory once and retain only directory summaries");
}
void thousandArchiveGroupsVisitIndexOnce() {
  TempDirectory temp;
  const auto archive = temp.root / "pack.zip";
  std::vector<std::pair<std::string, std::string>> files;
  InventoryRequest request;
  for (int i = 0; i < 1000; ++i) {
    const auto folder = "song" + std::to_string(i);
    files.emplace_back(folder + "/sound.wav", "12345");
    files.emplace_back(folder + "/chart.bms", "123");
    request.groups.push_back(archiveGroup(folder, archive / folder));
  }
  request.groups.push_back(archiveGroup("all", archive));
  request.groups.push_back(archiveGroup("duplicate", archive / "song0"));
  writeArchive(archive, files);
  StorageScanMetrics metrics;
  const auto measured = MeasureStorageGroups(request, {}, {}, &metrics);
  require(measured.complete && measured.groups.size() == 1002, "archive measurements cover all virtual groups");
  require(measured.groups[0].totalBytes == 8 && measured.groups[999].audioBytes == 5 &&
              measured.groups[1000].totalBytes == 8000 && measured.groups[1001].totalBytes == 8,
          "one archive pass computes implicit, nested and duplicate folder sizes");
  require(metrics.archiveIndexVisits == 1 && metrics.archiveEntries == 2000 && metrics.metadataReads == 0,
          "thousands of groups share one archive index visit without file extraction");
  require(metrics.peakArchiveAggregationRecords <= request.groups.size(),
          "archive aggregation retains only selected rows, not every encountered directory");
  require(archive_file::residentArchiveIndexCountForTesting() == 0, "storage-only scan does not accumulate resident indexes");
}
void archiveRootsAreSourceScoped() {
  TempDirectory temp;
  const auto first = temp.root / "first.zip", second = temp.root / "second.zip";
  writeArchive(first, {{"pack/a.wav", "12"}, {"package/b.wav", "1234567"}});
  writeArchive(second, {{"pack/a.wav", "12345"}});
  InventoryRequest request;
  request.groups = {archiveGroup("one", first / "pack"), archiveGroup("two", second / "pack"),
                    archiveGroup("sibling", first / "package"), archiveGroup("trailing", first / "pack" / "")};
  auto measured = MeasureStorageGroups(request);
  require(measured.complete && measured.groups[0].totalBytes == 2 && measured.groups[1].totalBytes == 5 &&
              measured.groups[2].totalBytes == 7 && measured.groups[3].totalBytes == 2, "same inner names and prefix siblings stay source-scoped");
  request.groups = {archiveGroup("trailing", first / "pack" / "")};
  const auto trailing = MeasureStorageGroups(request);
  require(trailing.complete && trailing.groups[0].totalBytes == 2, "single trailing archive folder retains complete union totals");
}
void archiveInnerCaseRemainsDistinct() {
  TempDirectory temp;
  const auto archive = temp.root / "case.zip";
  writeArchive(archive, {{"Song/a.wav", "12"}, {"song/b.wav", "12345"}});
  InventoryRequest request;
  request.groups = {archiveGroup("upper", archive / "Song"), archiveGroup("lower", archive / "song")};
  const auto measured = MeasureStorageGroups(request);
  require(measured.complete && measured.groups.at(0).totalBytes == 2 && measured.groups.at(1).totalBytes == 5 &&
      measured.totalBytes == 7, "case-distinct archive folders retain separate rows and contribute to the union");
}
void solidArchivesStayExcluded() {
  TempDirectory temp;
  const auto archive = temp.root / "solid.7z";
  writeArchive(archive, {{"song/a.wav", "123"}, {"song/chart.bms", "123"}}, true);
  InventoryRequest request;
  request.groups = {archiveGroup("solid", archive / "song")};
  const auto measured = MeasureStorageGroups(request);
  require(!measured.complete && measured.groups.size() == 1 && !measured.groups[0].totalBytes &&
              measured.groups[0].reason.code == StorageReason::UnsupportedSource, "solid archives remain excluded chart sources");
}
void invalidCachePublishesNoTotals() {
  TempDirectory temp;
  const auto archive = temp.root / "pack.zip", cache = temp.root / "index";
  writeArchive(archive, {{"song/a.wav", "12345"}, {"song/b.wav", "123"}});
  archive_file::setArchiveIndexCacheDirectory(cache);
  InventoryRequest request;
  request.groups = {archiveGroup("one", archive / "song")};
  require(MeasureStorageGroups(request).groups.at(0).totalBytes == 8, "cache fixture measurement");
  const auto file = fs::directory_iterator(cache)->path();
  std::ifstream input(file, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  const auto second = bytes.find("song/b.wav");
  require(second != std::string::npos, "find second cached record");
  bytes[second] = '/';
  { std::ofstream output(file, std::ios::binary | std::ios::trunc); output.write(bytes.data(), bytes.size()); }
  const auto measured = MeasureStorageGroups(request);
  require(!measured.complete && !measured.groups.at(0).totalBytes, "partial invalid index never publishes misleading folder totals");
  require(MeasureStorageGroups(request).groups.at(0).totalBytes == 8,
          "invalid streamed cache is repaired for the next measurement without replaying partial totals");
  fs::resize_file(file, 20);
  require(MeasureStorageGroups(request).groups.at(0).totalBytes == 8,
          "truncated cache header rebuilds before any records are published");
  archive_file::setArchiveIndexCacheDirectory({});
}
void encryptedEntriesFollowDiscoveryRules() {
  TempDirectory temp;
  const auto archive = temp.root / "mixed.zip";
  { std::ofstream output(archive, std::ios::binary);
    output.write(reinterpret_cast<const char *>(archive_zip_fixtures::mixedEncryption),
                 sizeof(archive_zip_fixtures::mixedEncryption)); }
  InventoryRequest request;
  request.groups = {archiveGroup("mixed", archive)};
  const auto measured = MeasureStorageGroups(request);
  require(measured.complete && measured.groups.at(0).totalBytes == 73,
          "encrypted entries stay excluded using the existing discovery index");
}
void changedArchiveInvalidatesBorrowedMeasurements() {
  TempDirectory temp;
  const auto archive = temp.root / "changed.zip";
  writeArchive(archive, {{"song/a.wav", "12"}});
  std::string error;
  require(!archive_file::visitIndexEntries(archive, [&](const auto &) {
    std::ofstream output(archive, std::ios::binary | std::ios::app); output << "changed";
    return true;
  }, &error), "archive identity change prevents publishing a successful measurement");
}
void overflowAndCancellationStayUnavailable() {
  InventoryRequest request;
  request.groups = {{"synthetic", "/synthetic"}};
  StorageScanDependencies deps;
  deps.enumerate = [](const auto &, const auto &, auto, auto, auto visitor, auto &stamp, auto *, auto &) {
    stamp.fileId = 1;
    visitor({.name = "a.wav", .kind = StorageEntryKind::Regular, .bytes = std::numeric_limits<std::uint64_t>::max()});
    visitor({.name = "b.wav", .kind = StorageEntryKind::Regular, .bytes = 1});
    return true;
  };
  const auto overflow = MeasureStorageGroups(request, {}, {}, nullptr, deps);
  require(!overflow.complete && !overflow.groups.at(0).totalBytes, "overflow never wraps into a plausible size");
  const auto cancelled = MeasureStorageGroups(request, [] { return false; }, {}, nullptr, deps);
  require(cancelled.cancelled && !cancelled.groups.at(0).totalBytes, "cancelled scan leaves unknown totals");
}
}
int RunStorageMeasurementTests() {
  try {
    archive_file::clearArchiveIndexCacheForTesting();
    sizeOnlyDoesNotBuildPreflight();
    thousandArchiveGroupsVisitIndexOnce();
    archiveRootsAreSourceScoped();
    archiveInnerCaseRemainsDistinct();
    solidArchivesStayExcluded();
    invalidCachePublishesNoTotals();
    encryptedEntriesFollowDiscoveryRules();
    changedArchiveInvalidatesBorrowedMeasurements();
    overflowAndCancellationStayUnavailable();
  } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
  return 0;
}
