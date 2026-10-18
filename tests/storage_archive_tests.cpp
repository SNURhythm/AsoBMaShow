#include "../src/storage/StorageOptimizer.h"
#include "../src/storage/StorageArchive.h"
#include "../src/storage/StorageArchiveTransaction.h"
#include "../src/storage/StorageArchiveDb.h"
#include "../src/audio/decoder.h"
#include "../src/ArchiveRAII.h"
#include "../src/ArchiveFile.h"
#include "StorageTestSupport.h"
#include <archive_entry.h>
#include <zlib.h>
#include <iostream>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

using namespace storage_management;
using storage_test::require;
namespace fs = std::filesystem;
namespace {
std::map<std::string, std::string> readArchive(const fs::path &path);
void writeArchive(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &files,
    bool deflated = false, bool tar = false) {
  auto writer = makeArchiveWriteHandle();
  require((tar ? archive_write_set_format_ustar(writer.get()) : archive_write_set_format_zip(writer.get())) == ARCHIVE_OK &&
      (tar || archive_write_set_options(writer.get(), deflated ? "zip:compression=deflate" : "zip:compression=store") == ARCHIVE_OK) &&
      archive_write_open_filename(writer.get(), path.string().c_str()) == ARCHIVE_OK, "open fixture ZIP");
  for (const auto &[name, contents] : files) {
    auto *entry = archive_entry_new();
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, contents.size());
    const bool written = archive_write_header(writer.get(), entry) == ARCHIVE_OK &&
        archive_write_data(writer.get(), contents.data(), contents.size()) == static_cast<la_ssize_t>(contents.size());
    archive_entry_free(entry);
    require(written, "write fixture ZIP entry");
  }
  require(archive_write_close(writer.get()) == ARCHIVE_OK, "finish fixture ZIP");
}
void manifestsExtractEntries() {
  storage_test::TempDirectory temp;
  const std::vector<std::pair<std::string, std::string>> files = {{"music/a.wav", std::string(1024 * 300, 'a')},
      {"music/chart.bms", "#WAV01 a.wav\n#TITLE fixture"}, {"music/日本語.wav", std::string(1200, 'b')}};
  for (int variant = 0; variant < 3; ++variant) {
    const auto path = temp.root / (std::to_string(variant) + (variant == 2 ? ".tar" : ".zip"));
    writeArchive(path, files, variant == 1, variant == 2);
    StorageArchive archive(path, temp.root / (std::to_string(variant) + ".sqlite"), {});
    std::optional<std::uint64_t> cursor; unsigned count = 0;
    while (auto entry = archive.Next(cursor)) {
      cursor = entry->offset;
      require(count < files.size() && entry->name == files[count].first, "manifest retains decoded filenames and entry order");
      const auto output = temp.root / (std::to_string(variant) + "-" + std::to_string(count));
      archive.Extract(*entry, output, {});
      std::ifstream stream(output, std::ios::binary); const std::string contents((std::istreambuf_iterator<char>(stream)), {});
      require(contents == files[count].second, "manifest extraction preserves payload bytes"); ++count;
    }
    require(count == files.size(), "manifest visits all stored/deflated/TAR entries");
  }
}
void selectedArchiveFoldersAreEligible() {
  storage_test::TempDirectory temp;
  const auto archive = temp.root / "library.zip";
  writeArchive(archive, {{"one/a.wav", std::string(1000, 'a')}, {"one/movie.avi", std::string(500, 'b')},
      {"one/chart.bms", std::string(50, 'c')}, {"two/b.wav", std::string(2000, 'd')},
      {"two/image.png", std::string(30, 'e')}});
  InventoryRequest request{.groups = {{"one", archive / "one", false, false, StorageSourceKind::Archive},
      {"two", archive / "two", false, false, StorageSourceKind::Archive},
      {"duplicate", archive / "one", false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete}};
  request.scanScratchRoot = temp.root / "private";
  const auto result = BuildStoragePreflight(request);
  require(result.complete && result.eligibleCount == 3 && result.eligibleBytes == 3500,
      "archive review counts WAV and video entries once across selected folders");
  require(result.totalBytes == 3580 && result.groups[0].totalBytes == 1550 && result.groups[1].totalBytes == 2030,
      "archive review reports logical group sizes and a deduplicated union");
  require(result.EligibleRoots() == std::vector<fs::path>{archive}, "archive admission reserves the container once");
}
void archiveReviewSeparatesPrivateAndLibraryAccess() {
  storage_test::TempDirectory temp;
  const auto library = temp.root / "Documents", privateRoot = temp.root / "ApplicationSupport";
  fs::create_directories(library); fs::create_directories(privateRoot);
  const auto archive = library / "library.zip";
  writeArchive(archive, {{"song/a.wav", std::string(1000, 'a')}, {"song/movie.avi", std::string(500, 'v')}});
  InventoryRequest request{.groups = {{"song", archive / "song", false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete},
      .pathAccessRoots = {library}, .scanScratchRoot = privateRoot / "storage-management/scan",
      .scanAccessRoot = privateRoot};
  const auto review = BuildStoragePreflight(request);
  require(review.complete && review.eligibleCount == 2 && review.eligibleBytes == 1500 &&
      review.groups.front().reason.code == StorageReason::None,
      "private archive index outside the library bookmark remains eligible");

  // The private grant must not authorize a source outside the library grant.
  const auto outside = privateRoot / "outside.zip";
  writeArchive(outside, {{"song/movie.avi", "video"}});
  request.groups.front().path = outside / "song";
  const auto rejected = BuildStoragePreflight(request);
  require(rejected.eligibleCount == 0 && rejected.groups.front().reason.code == StorageReason::UnsafePath,
      "private scratch access does not grant archive source access");

  request.groups.front().path = archive / "song";
  fs::create_directory_symlink(library, privateRoot / "linked");
  request.scanScratchRoot = privateRoot / "linked/scan";
  const auto linked = BuildStoragePreflight(request);
  require(linked.eligibleCount == 0 && linked.groups.front().reason.code == StorageReason::UnsafePath,
      "symlink beneath the private boundary still rejects scratch storage");
}
void reviewUsesCachedIndexAndApplyValidatesLayout() {
  storage_test::TempDirectory temp;
  archive_file::setArchiveIndexCacheDirectory(temp.root / "indexes");
  const auto path = temp.root / "songs.zip";
  writeArchive(path, {{"a.wav", std::string(1000, 'a')}, {"b.wav", std::string(1000, 'b')}});
  // Browsing reads ZIP's central directory. Review can reuse that index even
  // though a local header is invalid; Apply must validate physical records.
  {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    const std::string bytes((std::istreambuf_iterator<char>(file)), {});
    const auto second = bytes.find("PK\003\004", 4);
    require(second != std::string::npos, "find second local header");
    file.clear(); file.seekp(second); file.write("BAD!", 4);
  }
  require(archive_file::visitIndexEntries(path, [](const auto &) { return true; }), "prime browsing index after the edit");
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
  StorageScanMetrics metrics;
  const auto review = BuildStoragePreflight(request, {}, &metrics);
  require(review.complete && review.eligibleCount == 2 && metrics.archiveIndexVisits == 1 && metrics.archiveEntries == 2,
      "review consumes one cached index without rebuilding physical layout metadata");
  std::string error; const auto before = HashStorageFile(path, {}, error);
  unsigned encodes = 0;
  StorageArchiveDependencies deps{.encode = [&](const auto &, const auto &, const auto &, auto, auto) {
    ++encodes; return EncodeResult{};
  }};
  const auto result = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
  require(result.failed == 1 && !result.recoveryRequired && encodes == 0 && HashStorageFile(path, {}, error) == before,
      "Apply validates physical layout before encoding or writing despite a cached review");
}
void reviewAndApplyPreserveSystemEntries() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "songs.zip";
  writeArchive(path, {{"movie.avi", "video"}, {"._movie.avi", "sidecar"},
      {"__MACOSX/other.avi", "system"}, {"song/chart.bms", "chart"}});
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  require(review.eligibleCount == 1, "review excludes the browsing index's system entries");
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), {});
  const auto files = readArchive(path);
  require(report.deleted == 1 && !report.failed && files.size() == 3 &&
      files.at("._movie.avi") == "sidecar" && files.at("__MACOSX/other.avi") == "system" &&
      files.at("song/chart.bms") == "chart", "Apply changes exactly the reviewed media, preserving excluded system entries");
}
void archiveCompactionBatchesSmallRecords() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "songs.zip";
  const std::vector<std::pair<std::string, std::string>> files = {
      {"movie.avi", std::string(2000, 'v')}, {"one.png", std::string(300, 'a')},
      {"two.png", std::string(400, 'b')}, {"three.png", std::string(500, 'c')}, {"chart.bms", "#TITLE preserved"}};
  writeArchive(path, files);
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  unsigned advances = 0;
  StorageArchiveDependencies deps{.faultPoint = [&](std::string_view point) {
    if (point == "archive-cursor") ++advances;
  }};
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
  require(report.deleted == 1 && !report.failed && !report.recoveryRequired, "batched compaction completes");
  require(advances < 4, "small retained records share durable cursor advances instead of syncing each file");
  const auto actual = readArchive(path);
  require(actual.size() == 4, "only the selected video is deleted");
  for (std::size_t i = 1; i < files.size(); ++i)
    require(actual.at(files[i].first) == files[i].second, "batched compaction preserves each retained payload");
}
void archiveLargeRecordRecovery() {
  for (const auto &point : {"archive-redo", "archive-source-synced", "foreign-outside-redo", "foreign-after-truncate"}) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / "songs.zip";
    const std::string image(9 * 1024 * 1024 + 17, 'i');
    writeArchive(path, {{"movie.avi", std::string(2000, 'v')}, {"image.png", image}, {"chart.bms", "chart"}});
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    const std::string mode(point);
    StorageArchiveDependencies deps{.faultPoint = [&](std::string_view name) {
      if (name == mode || (mode == "foreign-outside-redo" && name == "archive-redo") ||
          (mode == "foreign-after-truncate" && name == "archive-truncate")) throw std::runtime_error("batch interruption");
    }};
    require(UpdateStorageArchive(review.archives.front(), request.policy, paths, deps).recoveryRequired,
        "interrupt an archive whose retained record exceeds the copy batch");
    if (mode.starts_with("foreign")) {
      StorageArchiveIO file(path, {}); file.Write(8 * 1024 * 1024 + 10000, "X"); file.Sync();
    }
    std::string error; const auto before = HashStorageFile(path, {}, error);
    const auto result = RecoverStorageArchives(paths);
    if (mode.starts_with("foreign")) {
      require(result.recoveryRequired && HashStorageFile(path, {}, error) == before,
          "foreign edits beyond redo ranges and after truncation block recovery without mutation");
    } else {
      require(!result.recoveryRequired && !result.failed && readArchive(path).at("image.png") == image,
          "recovery resumes a retained record across multiple bounded batches");
    }
  }
}
void archiveConsecutiveDeletesAreBatched() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "songs.zip";
  std::vector<std::pair<std::string, std::string>> files;
  for (unsigned i = 0; i < 260; ++i) files.emplace_back(std::to_string(i) + ".avi", "video");
  files.emplace_back("chart.bms", "chart"); writeArchive(path, files);
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  unsigned prepared = 0; StorageArchiveMetrics metrics;
  StorageArchiveDependencies deps{.faultPoint = [&](std::string_view point) {
    if (point == "archive-entry-ready") ++prepared;
  }, .metrics = &metrics};
  bool preparing = false, byteProgress = false;
  const auto result = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps, {}, [&](const StorageProgress &progress) {
        preparing |= progress.phase == StoragePhase::ArchivePrepare;
        byteProgress |= progress.phase == StoragePhase::Commit && progress.archiveBytesDone > 0 && progress.archiveBytesTotal > 0;
      });
  require(result.deleted == 260 && !result.failed && !result.recoveryRequired && readArchive(path).at("chart.bms") == "chart",
      "consecutive deletion batches preserve the retained chart");
  require(prepared <= 3 && metrics.durableBatches <= 4, "deletions share durable batches instead of syncing each action");
  require(preparing && byteProgress, "archive preparation and compaction publish distinct byte progress");
}
void archivePreparationDoesNotReadPayloads() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "songs.zip";
  writeArchive(path, {{"movie.avi", std::string(2 * 1024 * 1024, 'v')}, {"chart.bms", "#TITLE preserved"}});
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  std::string error; const auto before = HashStorageFile(path, {}, error);
  StorageArchiveMetrics metrics;
  StorageArchiveDependencies deps{.faultPoint = [](std::string_view point) {
    if (point == "archive-ready") throw StorageArchiveError(StorageReason::Cancelled, "stop before applying");
  }, .metrics = &metrics};
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
  require(report.cancelled && !report.recoveryRequired && HashStorageFile(path, {}, error) == before,
      "cancellation before applying keeps the original archive");
  require(metrics.preparationBytesRead < 128 * 1024,
      "confirmation reads archive metadata, not every payload byte");
}
void archiveTraversal(std::size_t count, bool enforceBudget) {
  using storage_management::archive_detail::number;
  storage_test::TempDirectory temp;
  const auto path = temp.root / "reverse-directory.zip";
  std::vector<std::pair<std::string, std::string>> files;
  for (std::size_t i = 0; i < count; ++i)
    files.emplace_back("song/" + std::to_string(i) + ".wav", std::string(2048, 'a'));
  writeArchive(path, files);
  // Central directory order is independent of physical local-record order.
  // Reverse only the central records to exercise a valid but seek-heavy ZIP.
  {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(-22, std::ios::end); std::string end(22, '\0'); file.read(end.data(), end.size());
    require(number(end, 0, 4) == 0x06054b50, "fixture has an ordinary ZIP end record");
    const auto offset = number(end, 16, 4);
    std::string central(number(end, 12, 4), '\0'); file.seekg(offset); file.read(central.data(), central.size());
    std::vector<std::string_view> records;
    for (std::size_t at = 0; at < central.size();) {
      const auto size = 46 + number(central, at + 28, 2) + number(central, at + 30, 2) + number(central, at + 32, 2);
      require(size <= central.size() - at, "complete fixture central record");
      records.push_back(std::string_view(central).substr(at, size)); at += size;
    }
    file.seekp(offset);
    for (auto i = records.rbegin(); i != records.rend(); ++i) file.write(i->data(), i->size());
    require(file.good(), "reverse fixture central directory");
  }
  const auto start = std::chrono::steady_clock::now();
  StorageArchive archive(path, temp.root / "manifest.sqlite", {});
  const auto prepared = std::chrono::steady_clock::now();
  const auto calls = archive.io.ReadCalls(), backwards = archive.io.BackwardReads(), bytes = archive.io.BytesRead();
  storage_management::archive_detail::Statement next(archive.db->value, storage_management::archive_detail::nextArchiveEntrySql);
  storage_management::archive_detail::Statement collisions(archive.db->value, storage_management::archive_detail::candidateCollisionSql);
  std::optional<std::uint64_t> cursor;
  std::size_t visited = 0;
  while (auto entry = storage_management::archive_detail::nextArchiveEntry(next, cursor)) {
    cursor = entry->offset;
    StorageDiagnostic reason;
    require(visited < files.size() && entry->name == files[visited].first && entry->bytes == 2048 &&
        storage_management::archive_detail::archiveCandidate(*entry, {.audio = AudioAction::LosslessFlac}, reason, collisions).has_value(),
        "physical traversal retains names, sizes and compression eligibility");
    ++visited;
  }
  const auto done = std::chrono::steady_clock::now();
  require(visited == count, "traversal retains every entry");
  std::cout << "archive traversal entries=" << count << " reads=" << calls << " backwards=" << backwards << " bytes=" << bytes
      << " prepare_ms=" << std::chrono::duration<double, std::milli>(prepared - start).count()
      << " traverse_ms=" << std::chrono::duration<double, std::milli>(done - prepared).count() << '\n';
  if (enforceBudget) {
    require(calls <= 3 * count + 8, "archive metadata reads are combined rather than split per field");
    require(backwards <= 3, "local validation walks physical offsets even when the central directory is reversed");
    require(bytes < 65557 + count * 300, "metadata traversal does not prefetch media payloads");
  }
}
void archiveTraversalUsesOrderedMetadataReads() { archiveTraversal(32, true); }
void archiveDescriptorBoundaries() {
  using storage_management::archive_detail::put;
  for (bool wide : {false, true}) for (bool signedDescriptor : {false, true}) for (int damage = 0; damage < 3; ++damage) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / "descriptors.zip";
    std::string contents;
    std::vector<std::string> central;
    const std::vector<std::pair<std::string, std::string>> files = {{"movie.avi", "video"}, {"chart.bms", "#TITLE retained"}};
    for (const auto &[name, payload] : files) {
      const auto offset = contents.size();
      const auto crc = crc32(0, reinterpret_cast<const Bytef *>(payload.data()), payload.size());
      std::string extra(wide ? 20 : 0, '\0');
      if (wide) { put(extra, 0, 1, 2); put(extra, 2, 16, 2); put(extra, 4, payload.size(), 8); put(extra, 12, payload.size(), 8); }
      std::string local(30, '\0'); put(local, 0, 0x04034b50, 4); put(local, 4, wide ? 45 : 20, 2); put(local, 6, 8, 2);
      put(local, 18, wide ? 0xffffffff : 0, 4); put(local, 22, wide ? 0xffffffff : 0, 4);
      put(local, 26, name.size(), 2); put(local, 28, extra.size(), 2);
      const unsigned prefix = signedDescriptor ? 4 : 0, sizeWidth = wide ? 8 : 4;
      std::string descriptor(prefix + 4 + 2 * sizeWidth, '\0');
      if (signedDescriptor) put(descriptor, 0, 0x08074b50, 4);
      put(descriptor, prefix, crc, 4); put(descriptor, prefix + 4, payload.size(), sizeWidth);
      put(descriptor, prefix + 4 + sizeWidth, payload.size(), sizeWidth);
      if (name == "chart.bms") {
        if (damage == 1) descriptor.pop_back();
        if (damage == 2) descriptor[prefix] ^= 1;
      }
      contents += local + name + extra + payload + descriptor;
      std::string row(46, '\0'); put(row, 0, 0x02014b50, 4); put(row, 4, 45, 2); put(row, 6, wide ? 45 : 20, 2);
      put(row, 8, 8, 2); put(row, 16, crc, 4); put(row, 20, payload.size(), 4); put(row, 24, payload.size(), 4);
      put(row, 28, name.size(), 2); put(row, 42, offset, 4); central.push_back(row + name);
    }
    const auto centralOffset = contents.size();
    for (auto row = central.rbegin(); row != central.rend(); ++row) contents += *row;
    std::string end(22, '\0'); put(end, 0, 0x06054b50, 4); put(end, 8, files.size(), 2); put(end, 10, files.size(), 2);
    put(end, 12, contents.size() - centralOffset, 4); put(end, 16, centralOffset, 4); contents += end;
    { std::ofstream output(path, std::ios::binary); output.write(contents.data(), contents.size()); }
    if (!damage) {
      StorageArchive archive(path, temp.root / "manifest.sqlite", {});
      const auto first = archive.Next(); require(first.has_value(), "first descriptor entry exists");
      const auto second = archive.Next(first->offset);
      require(first->name == "movie.avi" && second && second->name == "chart.bms" &&
          first->offset + first->span == second->offset && second->offset + second->span == centralOffset,
          "signed and unsigned classic/ZIP64 descriptors retain exact spans in physical order");
    }
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    require(review.archives.size() == 1, "central directory remains readable for descriptor fixture");
    const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
        MakeStoragePlatformPaths(temp.root / "private", {temp.root}), {});
    if (damage) {
      std::ifstream input(path, std::ios::binary); const std::string after((std::istreambuf_iterator<char>(input)), {});
      require(report.failed == 1 && !report.recoveryRequired && after == contents,
          "truncated and corrupt descriptors reject Apply before archive mutation");
    } else {
      const auto remaining = readArchive(path);
      require(report.deleted == 1 && !report.failed && remaining.size() == 1 && remaining.at("chart.bms") == files.back().second,
          "reverse central order and descriptor lookahead preserve the retained payload through compaction");
    }
  }
}
void archiveSavingsUsePhysicalSize() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "compressed.zip";
  writeArchive(path, {{"movie.avi", std::string(100000, 'v')}, {"chart.bms", "#TITLE retained"}}, true);
  const auto before = fs::file_size(path);
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), {});
  const auto after = BuildStoragePreflight(request);
  require(report.deleted == 1 && !report.failed && report.reclaimedBytes == before - fs::file_size(path) &&
      review.totalBytes && after.totalBytes && *review.totalBytes - *after.totalBytes == 100000 && report.reclaimedBytes < 1000,
      "archive row changes count uncompressed contents while reclaimed space counts physical ZIP reduction");
}
std::map<std::string, std::string> readArchive(const fs::path &path) {
  auto reader = makeArchiveReadHandle(); archive_read_support_filter_all(reader.get()); archive_read_support_format_all(reader.get());
  require(archive_read_open_filename(reader.get(), path.string().c_str(), 65536) == ARCHIVE_OK, "open updated archive with independent reader");
  std::map<std::string, std::string> files; archive_entry *entry = nullptr; int status;
  while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
    auto &contents = files[archive_entry_pathname(entry)]; std::array<char, 65536> buffer{}; la_ssize_t got;
    while ((got = archive_read_data(reader.get(), buffer.data(), buffer.size())) > 0) contents.append(buffer.data(), got);
    require(got == 0, "updated archive payload is readable and checksum-valid");
  }
  require(status == ARCHIVE_EOF, "updated archive has a valid end record"); return files;
}
void archiveUpdatesBatchWithoutFullCopy() {
  for (bool tar : {false, true}) {
  storage_test::TempDirectory temp;
  const auto archive = temp.root / (tar ? "library.tar" : "library.zip");
  const std::string image(700000, 'i');
  writeArchive(archive, {{"one/a.wav", std::string(10000, 'a')}, {"one/movie.avi", std::string(9000, 'v')},
      {"one/chart.bms", "#WAV01 a.wav"}, {"two/image.png", image}}, false, tar);
  const auto before = fs::file_size(archive);
  InventoryRequest request{.groups = {{"one", archive / "one", false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  require(review.archives.size() == 1, "review supplies one archive batch");
  StorageArchiveMetrics metrics;
  StorageArchiveDependencies dependencies{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
    std::ofstream file(output, std::ios::binary); file << std::string(1000, 'f'); file.close();
    return EncodeResult{.outcome = FileOutcome::Converted, .outputPath = output};
  }, .metrics = &metrics};
  const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy, paths, dependencies);
  require(report.converted == 1 && report.deleted == 1 && !report.failed && !report.recoveryRequired,
      "one archive batch converts selected audio and deletes selected video");
  const auto files = readArchive(archive);
  require(files.size() == 3 && files.at("one/a.flac") == std::string(1000, 'f') &&
      files.at("one/chart.bms") == "#WAV01 a.wav" && files.at("two/image.png") == image,
      "archive update preserves authored references and unselected payloads");
  require(report.changedRoots == std::vector<fs::path>{archive} && metrics.archivesCompacted == 1,
      "archive changes report and compact one physical source");
  require(report.reclaimedBytes == before - fs::file_size(archive) && metrics.peakScratchBytes < before / 2,
      "archive savings use physical bytes with bounded per-entry staging");
  require(metrics.sourceBytesRead < before * 5 / 2, "compaction does not reread the whole archive at completion");
  }
}
void interruptedArchiveWritesRecover() {
  const std::vector<std::pair<std::string, unsigned>> faults = {{"archive-entry-ready", 1}, {"archive-redo", 2},
      {"archive-write", 3}, {"archive-source-synced", 1}, {"archive-cursor", 2}, {"archive-before-truncate", 1}, {"archive-truncate", 1},
      {"archive-truncate-synced", 1}, {"archive-cleanup-ready", 1}, {"archive-manifest-unlinked", 1}, {"archive-cleanup-remove", 1}};
  for (const auto &[point, occurrence] : faults) {
    storage_test::TempDirectory temp;
    const auto archive = temp.root / "library.zip";
    std::string image(700000, 'i');
    for (std::size_t i = 0; i < image.size(); ++i) image[i] = static_cast<char>((i * 17) % 251);
    writeArchive(archive, {{"one/a.wav", std::string(10000, 'a')}, {"one/movie.avi", std::string(9000, 'v')},
        {"one/chart.bms", "#WAV01 a.wav"}, {"two/image.png", image}});
    InventoryRequest request{.groups = {{"one", archive / "one", false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request); const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    unsigned calls = 0;
    StorageArchiveDependencies dependencies{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      std::ofstream file(output, std::ios::binary); file << std::string(1000, 'f'); file.close();
      return EncodeResult{.outcome = FileOutcome::Converted, .outputPath = output};
    }, .faultPoint = [&](std::string_view name) { if (name == point && ++calls == occurrence) throw std::runtime_error("injected archive interruption"); }};
    const auto interrupted = UpdateStorageArchive(review.archives.front(), request.policy, paths, dependencies);
    if (calls != occurrence || !interrupted.recoveryRequired) throw std::runtime_error(point + ": missing injected interruption (" + std::to_string(calls) + ")");
    std::string error; const auto beforeRecovery = HashStorageFile(archive, {}, error);
    const auto recovered = RecoverStorageArchives(paths);
    if (point == "archive-write" || point == "archive-truncate") {
      require(recovered.recoveryRequired && HashStorageFile(archive, {}, error) == beforeRecovery,
          "ambiguous source writes retain recovery evidence without another mutation");
      continue;
    }
    if (recovered.recoveryRequired && !recovered.files.empty()) throw std::runtime_error(point + ": " + recovered.files.front().reason.detail);
    require(!recovered.failed && !recovered.recoveryRequired, "durable archive recovery finishes without encoding again");
    const auto files = readArchive(archive);
    require(files.at("one/a.flac") == std::string(1000, 'f') && files.at("two/image.png") == image &&
        files.at("one/chart.bms") == "#WAV01 a.wav", "recovery preserves completed media and overlapping copied payloads");
    require(fs::is_empty(paths.journalRoot), "successful archive recovery removes its owned journal");
    require(RecoverStorageArchives(paths).changedRoots.empty(), "archive recovery is idempotent");
  }
}
void writeSparseZip64(const fs::path &path, std::uint64_t offset = 128ULL * 1024 * 1024 * 1024 + 4096,
    const std::string &audioName = "\x93\xfa\x96\x7b\x8c\xea.wav", bool prefix = false) {
  using storage_management::archive_detail::put;
  std::ofstream file(path, std::ios::binary);
  file.seekp(offset); std::string central;
  std::vector<std::pair<std::string, std::string>> files = {
      {"movie.avi", std::string(1000, 'v')}, {audioName, std::string(10000, 'a')}, {"chart.bms", "fixture"}};
  if (prefix) files.insert(files.begin(), {"prefix.png", "prefix"});
  for (const auto &[name, payload] : files) {
    const auto crc = crc32(0, reinterpret_cast<const Bytef *>(payload.data()), payload.size());
    std::string local(30, '\0'); put(local, 0, 0x04034b50, 4); put(local, 4, 20, 2); put(local, 14, crc, 4);
    put(local, 18, payload.size(), 4); put(local, 22, payload.size(), 4); put(local, 26, name.size(), 2);
    const auto record = local + name + payload; file.write(record.data(), record.size());
    std::string row(46, '\0'); put(row, 0, 0x02014b50, 4); put(row, 4, 45, 2); put(row, 6, 45, 2); put(row, 16, crc, 4);
    put(row, 20, payload.size(), 4); put(row, 24, payload.size(), 4); put(row, 28, name.size(), 2); put(row, 30, 12, 2);
    put(row, 32, 10, 2); put(row, 42, 0xffffffff, 4);
    std::string extra(12, '\0'); put(extra, 0, 1, 2); put(extra, 2, 8, 2); put(extra, 4, offset, 8);
    central += row + name + extra + "entry-note"; offset += record.size();
  }
  file.write(central.data(), central.size());
  std::string end64(56, '\0'); put(end64, 0, 0x06064b50, 4); put(end64, 4, 44, 8); put(end64, 12, 45, 2); put(end64, 14, 45, 2);
  put(end64, 24, files.size(), 8); put(end64, 32, files.size(), 8); put(end64, 40, central.size(), 8); put(end64, 48, offset, 8);
  std::string locator(20, '\0'); put(locator, 0, 0x07064b50, 4); put(locator, 8, offset + central.size(), 8); put(locator, 16, 1, 4);
  std::string end(22, '\0'); put(end, 0, 0x06054b50, 4); put(end, 8, files.size(), 2); put(end, 10, files.size(), 2);
  put(end, 12, central.size(), 4); put(end, 16, 0xffffffff, 4); put(end, 20, 12, 2);
  const auto tail = end64 + locator + end + "archive-note"; file.write(tail.data(), tail.size()); require(file.good(), "write sparse ZIP64 fixture");
}
void sparsePrefixRecovery() {
  for (bool prefix : {false, true}) for (bool convert : {false, true})
  for (const auto point : {"", "archive-redo", "archive-source-synced"}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "songs.zip";
    writeSparseZip64(path, 4096, "audio.wav", prefix);
    const std::string stub = "MZ executable stub and preserved leading metadata";
    { std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary); file.write(stub.data(), stub.size()); }
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.bga = convert ? BgaAction::CompressH264 : BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    StorageArchiveDependencies deps{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      std::ofstream(output, std::ios::binary) << "smaller video"; return EncodeResult{FileOutcome::Converted, output};
    }, .faultPoint = [&](std::string_view name) {
      if (name == point) throw std::runtime_error("sparse batch interruption");
    }};
    const auto updated = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
    if (*point) require(updated.recoveryRequired, "interrupt a prefixed archive at a resumable checkpoint");
    else require(!updated.failed && (convert ? updated.converted : updated.deleted) == 1, "apply changes to a prefixed archive");
    const auto result = RecoverStorageArchives(paths);
    require(!result.failed && !result.recoveryRequired, "prefixed archive cursor remains stable across recovery");
    std::ifstream leading(path, std::ios::binary); std::string preserved(stub.size(), '\0'); leading.read(preserved.data(), preserved.size());
    require(preserved == stub, "deleting or replacing the first local entry preserves leading ZIP bytes");
    StorageArchive archive(path, temp.root / "checked.sqlite", {});
    std::optional<std::uint64_t> cursor; unsigned count = 0;
    while (const auto entry = archive.Next(cursor)) {
      cursor = entry->offset; ++count;
      if (count == 1) require(entry->offset == 4096, "the first output record stays beyond the leading ZIP prefix");
      require(entry->name != "movie.avi", "recovery does not retain the selected original video");
      archive.Extract(*entry, temp.root / (std::to_string(count) + ".bin"), {});
    }
    require(count == 2 + unsigned(prefix) + unsigned(convert), "prefix preservation retains exactly the expected archive entries");
    require(RecoverStorageArchives(paths).changedRoots.empty(), "prefixed archive recovery is idempotent");
  }
}
void malformedLegacyNamesAreRejected() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "invalid.zip";
  // A CP932 lead byte followed by ASCII '.' must not become a replacement
  // character and silently name a different asset on Android or Windows.
  writeSparseZip64(path, 0, "\x81.wav");
  bool rejected = false;
  try { StorageArchive archive(path, temp.root / "invalid.sqlite", {}); }
  catch (const StorageArchiveError &) { rejected = true; }
  require(rejected, "malformed CP932 names are rejected instead of substituted");
}
void sparseZip64AndLegacyNames() {
  storage_test::TempDirectory temp; const auto path = temp.root / "large.zip"; writeSparseZip64(path);
  const auto prefixBytes = 128ULL * 1024 * 1024 * 1024 + 4096;
  StorageArchiveMetrics metrics;
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  require(review.eligibleCount == 2 && review.archives.size() == 1, "ZIP64 offsets and CP932 names remain eligible");
  StorageArchiveDependencies dependencies{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
    std::ofstream(output, std::ios::binary) << std::string(1000, 'f'); return EncodeResult{FileOutcome::Converted, output};
  }, .metrics = &metrics};
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy, MakeStoragePlatformPaths(temp.root / "private", {temp.root}), dependencies);
  require(report.converted == 1 && report.deleted == 1 && !report.recoveryRequired &&
      fs::file_size(path) > prefixBytes && fs::file_size(path) - prefixBytes < 5000 && metrics.sourceBytesRead < 128 * 1024,
      "ZIP64 compaction preserves a huge sparse prefix without reading or staging the hole");
  StorageArchive archive(path, temp.root / "checked.sqlite", {});
  require(archive.comment == "archive-note", "archive comment survives ZIP64 compaction");
  const auto first = archive.Next(); require(first && first->name == "日本語.flac" && first->metadata.ends_with("entry-note"),
      "CP932 stem and entry comment survive renaming");
  archive.Extract(*first, temp.root / "decoded.flac", {});
  std::ifstream input(temp.root / "decoded.flac", std::ios::binary); const std::string bytes((std::istreambuf_iterator<char>(input)), {});
  require(bytes == std::string(1000, 'f'), "rewritten legacy-named payload verifies its CRC");
}
void realArchiveOptimizerPreservesPlayback() {
  using storage_management::archive_detail::put;
  storage_test::TempDirectory temp; const auto path = temp.root / "songs.zip";
  std::string wav(44 + 32000, '\0'); wav.replace(0, 4, "RIFF"); put(wav, 4, wav.size() - 8, 4); wav.replace(8, 8, "WAVEfmt ");
  put(wav, 16, 16, 4); put(wav, 20, 1, 2); put(wav, 22, 1, 2); put(wav, 24, 16000, 4); put(wav, 28, 32000, 4);
  put(wav, 32, 2, 2); put(wav, 34, 16, 2); wav.replace(36, 4, "data"); put(wav, 40, 32000, 4);
  for (std::size_t i = 0; i < 16000; ++i) put(wav, 44 + 2 * i, i % 100, 2);
  std::string metadata(32, '\0'); metadata.replace(0, 4, "LIST"); put(metadata, 4, 24, 4);
  metadata.replace(8, 8, "INFOISFT"); put(metadata, 16, 12, 4); metadata.replace(20, 11, "Wave editor");
  wav += metadata; put(wav, 4, wav.size() - 8, 4);
  writeArchive(path, {{"songs/tone.wav", wav}, {"songs/video.avi", std::string(1000, 'v')}, {"songs/chart.bms", "#WAV01 tone.wav"}, {"other/image.png", "image"}});
  std::vector<short> originalPcm; SF_INFO originalInfo{}; std::atomic_bool originalCancelled = false;
  require(decodeAudioToPCM(fspath_to_path_t(path / "songs/tone.wav"), originalPcm, originalInfo, originalCancelled), "decode original archive audio");
  OptimizationRequest request; request.policy = request.inventory.policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete};
  request.inventory.groups = {{"songs", path / "songs", false, false, StorageSourceKind::Archive}, {"overlap", path / "songs", false, false, StorageSourceKind::Archive}};
  request.inventory.scanScratchRoot = temp.root / "scan"; request.preflight = BuildStoragePreflight(request.inventory);
  auto dependencies = DefaultOptimizerDependencies(MakeStoragePlatformPaths(temp.root / "private", {temp.root}));
  dependencies.now = [] { return std::chrono::steady_clock::time_point{}; };
  bool preparing = false;
  const auto report = StorageOptimizer(std::move(dependencies)).Run(request, {}, [&](const StorageProgress &progress) {
    require(progress.phase != StoragePhase::Inventory && progress.filesTotal == request.preflight->eligibleCount,
        "confirmed archive apply keeps reviewed totals without publishing a folder rescan");
    preparing |= progress.phase == StoragePhase::ArchivePrepare;
  });
  require(preparing, "archive preparation bypasses the throttle before the metadata pass");
  if (report.failed && !report.files.empty()) throw std::runtime_error(report.files.front().reason.detail);
  require(report.converted == 1 && report.deleted == 1 && report.changedRoots == std::vector<fs::path>{path}, "optimizer batches overlapping archive folders through real codecs");
  const auto files = readArchive(path); require(files.contains("songs/tone.flac") && files.at("songs/chart.bms") == "#WAV01 tone.wav" && files.at("other/image.png") == "image", "real codec archive retains chart and sibling bytes");
  std::vector<short> pcm; SF_INFO info{}; std::atomic_bool cancelled = false;
  require(decodeAudioToPCM(fspath_to_path_t(path / "songs/tone.wav"), pcm, info, cancelled) && pcm.size() == 16000 && info.samplerate == 16000,
      "gameplay resolves the authored WAV name inside the updated archive");
  require(pcm == originalPcm, "archive FLAC preserves gameplay samples exactly");
}
void archivePolicyBoundaries() {
  for (const std::string mode : {"compressed", "budget", "cancel", "failed", "throw"}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "songs.zip";
    writeArchive(path, {{"a.wav", std::string(10000, 'a')}, {"b.wav", std::string(9000, 'b')}, {"chart.bms", "chart"}}, mode == "compressed");
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    std::string error; const auto before = ReadFileIdentity(path, error); bool cancel = false; unsigned calls = 0;
    StorageArchiveDependencies dependencies{.encode = [&](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      ++calls;
      if (calls == 2 && mode == "throw") throw std::runtime_error("injected encoder exception");
      if (calls == 2 && mode == "failed") return EncodeResult{.outcome = FileOutcome::Failed, .reason = {StorageReason::ValidationFailed, "injected validation failure"}};
      std::ofstream(output, std::ios::binary) << std::string(1000, 'f'); return EncodeResult{FileOutcome::Converted, output};
    }, .availableSpace = [&](const fs::path &) -> std::optional<std::uint64_t> { return mode == "budget" ? 1024 : 100000000; }};
    const auto result = UpdateStorageArchive(review.archives.front(), request.policy, paths, dependencies, [&] { return !cancel; },
        [&](const StorageProgress &progress) { if (mode == "cancel" && progress.phase == StoragePhase::Commit) cancel = true; });
    require(!result.recoveryRequired && fs::is_empty(paths.journalRoot), "ordinary skips, cancellation and encoder failures settle the archive");
    const auto files = readArchive(path); require(files.at("chart.bms") == "chart", "policy boundary preserves charts");
    if (mode == "compressed" || mode == "budget") {
      require(result.converted == 0 && result.skipped == 2 && ReadFileIdentity(path, error) == before,
          "not-smaller and insufficient-space batches do not modify the archive");
      if (mode == "budget") require(calls == 0, "staging budget is enforced before extraction or encoding");
    } else {
      require(result.converted == 1 && files.at("a.flac") == std::string(1000, 'f') && files.at("b.wav") == std::string(9000, 'b'),
          "cancellation or failed encoding retains future originals and accepted changes");
      require(mode == "cancel" ? result.cancelled : result.failed == 1, "boundary result reports cancellation or codec failure");
    }
  }
}
void unsafeAndStaleArchivesAreRejected() {
  for (const std::string mode : {"encrypted", "traversal", "collision", "unsupported", "stale"}) {
    storage_test::TempDirectory temp; const auto path = temp.root / (mode == "unsupported" ? "songs.7z" : "songs.zip");
    writeArchive(path, {{mode == "traversal" ? "../a.wav" : "a.wav", std::string(10000, 'a')}, {mode == "collision" ? "a.flac" : "chart.bms", "fixture"}});
    if (mode == "encrypted") {
      std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
      std::string bytes((std::istreambuf_iterator<char>(file)), {});
      bytes[6] |= 1; const auto central = bytes.find("PK\001\002"); require(central != std::string::npos, "find fixture central record"); bytes[central + 8] |= 1;
      file.clear(); file.seekp(0); file.write(bytes.data(), bytes.size());
    }
    if (mode == "unsupported") { std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out); file.write("7z\xbc\xaf", 4); }
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    if ((mode == "encrypted" || mode == "traversal") && review.eligibleCount) {
      std::string error; const auto before = HashStorageFile(path, {}, error);
      unsigned encodes = 0;
      StorageArchiveDependencies deps{.encode = [&](const auto &, const auto &, const auto &, auto, auto) {
        ++encodes; return EncodeResult{};
      }};
      const auto result = UpdateStorageArchive(review.archives.front(), request.policy,
          MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
      require(result.failed == 1 && !result.recoveryRequired && encodes == 0 && HashStorageFile(path, {}, error) == before,
          "indexed candidates with unsafe layouts are rejected before encoding or mutation");
    } else if (mode != "stale") {
      require(review.eligibleCount == 0 && review.EligibleRoots().empty(), "unsupported, ambiguous or unsafe archives have no mutation admission");
    } else {
      std::ofstream(path, std::ios::binary | std::ios::app) << 'x';
      std::string error; const auto changed = HashStorageFile(path, {}, error);
      const auto result = UpdateStorageArchive(review.archives.front(), request.policy, MakeStoragePlatformPaths(temp.root / "private", {temp.root}), {});
      require(result.failed == 1 && !result.recoveryRequired && HashStorageFile(path, {}, error) == changed && result.files.front().reason.code == StorageReason::StaleInput,
          "stale archive review cannot authorize a write");
    }
  }
}
void partialWritesAndForeignChanges() {
  for (bool foreign : {false, true}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "songs.zip";
    writeArchive(path, {{"a.wav", std::string(10000, 'a')}, {"image.png", std::string(700000, 'i')}, {"chart.bms", "chart"}});
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request); const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    unsigned calls = 0;
    StorageArchiveDependencies dependencies{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      std::ofstream(output, std::ios::binary) << std::string(1000, 'f'); return EncodeResult{FileOutcome::Converted, output};
    }, .faultPoint = [&](std::string_view point) {
      if ((foreign && point == "archive-entry-ready") || (!foreign && point == "archive-redo" && ++calls == 2)) throw std::runtime_error("injected write stop");
    }};
    require(UpdateStorageArchive(review.archives.front(), request.policy, paths, dependencies).recoveryRequired, "retain recovery fixture");
    const auto journal = fs::directory_iterator(paths.journalRoot)->path();
    std::uint64_t foreignOffset = 0;
    {
      archive_detail::Database db(journal / "manifest.sqlite", false);
      if (foreign) {
        archive_detail::Statement q(db.value, "SELECT data_offset FROM entries WHERE name='image.png'"); require(q.step(), "find untouched source"); foreignOffset = q.integer(0);
        StorageArchiveIO file(path, {}); file.Write(foreignOffset, "X"); file.Sync();
      } else {
        archive_detail::Statement q(db.value, "SELECT target,scratch_offset,length FROM redo JOIN chunks ON chunks.id=redo.chunk ORDER BY target LIMIT 1");
        require(q.step(), "find overlapping redo");
        StorageArchiveIO scratch(journal / "redo.bin", {}, false), file(path, {});
        const auto bytes = scratch.Read(q.integer(1) + q.integer(2), q.integer(2));
        file.Write(q.integer(0), std::string_view(bytes).substr(0, bytes.size() / 2)); file.Sync();
      }
    }
    std::string error; const auto before = HashStorageFile(path, {}, error); auto result = RecoverStorageArchives(paths);
    if (foreign) {
      require(result.recoveryRequired && HashStorageFile(path, {}, error) == before, "foreign source changes block recovery before another write");
      { StorageArchiveIO file(path, {}); file.Write(foreignOffset, "i"); file.Sync(); }
      const auto restored = HashStorageFile(path, {}, error);
      result = RecoverStorageArchives(paths);
      require(result.recoveryRequired && HashStorageFile(path, {}, error) == restored,
          "restoring bytes does not silently authorize an externally modified archive");
      continue;
    }
    require(result.recoveryRequired && HashStorageFile(path, {}, error) == before,
        "uncheckpointed partial writes remain fail-closed without a whole-archive scan");
  }
}
void archiveStagingRespectsPrivateBoundary() {
  storage_test::TempDirectory temp;
  fs::create_directories(temp.root / "system/app/private");
  fs::create_directory_symlink(temp.root / "system", temp.root / "system-alias");
  const auto library = temp.root / "library", privateRoot = temp.root / "system-alias/app/private";
  fs::create_directory(library);
  // A system alias above the trusted boundary models an ancestor that the app
  // must not inspect. Private descendants must still be checked for symlinks.
  std::string boundaryError;
  require(!IsSafeLocalPath(privateRoot, boundaryError) && IsSafeLocalPath(privateRoot, boundaryError, {privateRoot}),
      "fixture requires starting validation at the granted private boundary");
  const auto paths = MakeStoragePlatformPaths(privateRoot, {library}, true);
  for (const std::string mode : {"complete", "recover", "symlink"}) {
    const auto path = library / (mode + ".zip");
    writeArchive(path, {{"a.wav", std::string(10000, 'a')}, {"b.wav", std::string(9000, 'b')}, {"chart.bms", "chart"}});
    std::string error;
    const auto identity = ReadFileIdentity(path, error, {library});
    require(identity.has_value(), "archive identity is accessible inside the library grant");
    const StorageArchiveSummary summary{.path = path, .identity = *identity, .innerRoots = {fs::path{}}, .eligibleCount = 2};
    StorageArchiveDependencies deps{.encode = [&](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      if (mode == "symlink") {
        std::ofstream(privateRoot / "foreign") << std::string(1000, 'f');
        fs::create_symlink(privateRoot / "foreign", output);
      } else std::ofstream(output, std::ios::binary) << std::string(1000, 'f');
      return EncodeResult{FileOutcome::Converted, output};
    }, .faultPoint = [&](std::string_view point) {
      if (mode == "recover" && point == "archive-redo") throw std::runtime_error("simulated interruption");
    }};
    const auto result = UpdateStorageArchive(summary, {.audio = AudioAction::LosslessFlac}, paths, deps);
    if (mode == "complete") {
      if (result.failed && !result.files.empty()) throw std::runtime_error(result.files.front().reason.detail);
      require(result.converted == 2 && !result.recoveryRequired && fs::is_empty(paths.journalRoot),
          "consecutive archive conversions stay within authorized private storage");
      const auto files = readArchive(path);
      require(files.at("a.flac") == std::string(1000, 'f') && files.at("b.flac") == std::string(1000, 'f') &&
          files.at("chart.bms") == "chart", "sandboxed compaction retains converted media and charts");
    } else if (mode == "recover") {
      require(result.recoveryRequired, "fixture leaves a durable archive redo batch");
      const auto recovered = RecoverStorageArchives(paths);
      require(!recovered.failed && !recovered.recoveryRequired && recovered.converted == 1 && fs::is_empty(paths.journalRoot),
          "archive recovery reads private replacement and redo files without inspecting system parents");
      const auto files = readArchive(path);
      require(files.at("a.flac") == std::string(1000, 'f') && files.at("b.wav") == std::string(9000, 'b') &&
          files.at("chart.bms") == "chart", "recovery preserves accepted conversion and untouched entries");
    } else {
      require(result.failed == 1 && !result.recoveryRequired && result.converted == 0,
          "trusted private root never permits a symlink in staged output");
      require(readArchive(path).at("a.wav") == std::string(10000, 'a') && fs::file_size(privateRoot / "foreign") == 1000,
          "unsafe staged output leaves original and foreign files untouched");
    }
  }
}
}
void storageArchiveTraversalBenchmark(std::size_t count) { archiveTraversal(count, false); }
void storageArchiveDiscoverySandboxTest(const fs::path &root) {
  std::error_code error;
  (void)fs::symlink_status(root.parent_path(), error);
  require(error == std::errc::operation_not_permitted || error == std::errc::permission_denied,
      "fixture denies metadata access above the library grant");
  const auto privateRoot = root.parent_path().parent_path() / "private";
  archive_file::setArchiveIndexCacheDirectory(privateRoot / "indexes");
  const auto archive = root / "nested.zip/songs.zip";
  for (const auto &grant : {root, archive}) {
    InventoryRequest request{.groups = {{"song", archive / "song", false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .pathAccessRoots = {grant},
        .scanScratchRoot = privateRoot / "scan", .scanAccessRoot = privateRoot};
    const auto review = BuildStoragePreflight(request);
    if (review.eligibleCount != 1 && !review.groups.empty()) throw std::runtime_error(review.groups.front().reason.detail);
    require(review.complete && review.archives.size() == 1 && review.archives.front().path == archive &&
        review.archives.front().innerRoots == std::vector<fs::path>{"song"},
        "archive review resolves folder and file grants without probing system ancestors");
  }
}
void storageArchiveTests(const char *executable) {
#if defined(__APPLE__) && TARGET_OS_OSX
  storage_test::TempDirectory temp;
  fs::create_directories(temp.root / "system/library/nested.zip");
  fs::create_directory(temp.root / "private");
  writeArchive(temp.root / "system/library/nested.zip/songs.zip", {{"song/a.wav", std::string(10000, 'a')}});
  const auto profile = "(version 1) (allow default) (deny file-read-metadata (literal \"" +
      (temp.root / "system").string() + "\"))";
  const auto child = fork();
  require(child >= 0, "start sandboxed archive discovery subprocess");
  if (!child) {
    execl("/usr/bin/sandbox-exec", "sandbox-exec", "-p", profile.c_str(), executable,
        "--archive-discovery-sandbox", (temp.root / "system/library").c_str(), nullptr);
    _exit(127);
  }
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
      "archive review respects sandbox boundaries");
#else
  (void)executable;
#endif
  for (const auto &[name, run] : std::vector<std::pair<const char *, void(*)()>>{
      {"sparse-prefix-recovery", sparsePrefixRecovery}, {"large-batch-recovery", archiveLargeRecordRecovery}, {"delete-batches", archiveConsecutiveDeletesAreBatched},
      {"manifest", manifestsExtractEntries}, {"preflight", selectedArchiveFoldersAreEligible},
      {"sandbox", archiveReviewSeparatesPrivateAndLibraryAccess}, {"sandbox-staging", archiveStagingRespectsPrivateBoundary},
      {"cached-review", reviewUsesCachedIndexAndApplyValidatesLayout},
      {"system-entries", reviewAndApplyPreserveSystemEntries},
      {"metadata-preparation", archivePreparationDoesNotReadPayloads},
      {"metadata-traversal", archiveTraversalUsesOrderedMetadataReads},
      {"descriptor-boundaries", archiveDescriptorBoundaries},
      {"physical-savings", archiveSavingsUsePhysicalSize},
      {"batched-compaction", archiveCompactionBatchesSmallRecords},
      {"update", archiveUpdatesBatchWithoutFullCopy}, {"recovery", interruptedArchiveWritesRecover},
      {"zip64", sparseZip64AndLegacyNames}, {"invalid-encoding", malformedLegacyNamesAreRejected},
      {"playback", realArchiveOptimizerPreservesPlayback},
      {"boundaries", archivePolicyBoundaries}, {"rejections", unsafeAndStaleArchivesAreRejected},
      {"partial", partialWritesAndForeignChanges}}) {
    try { run(); } catch (const std::exception &error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
  }
}
