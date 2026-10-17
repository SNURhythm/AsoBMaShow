#include "../src/storage/StorageOptimizer.h"
#include "../src/storage/StorageArchive.h"
#include "../src/storage/StorageArchiveTransaction.h"
#include "../src/storage/StorageArchiveDb.h"
#include "../src/audio/decoder.h"
#include "../src/ArchiveRAII.h"
#include "StorageTestSupport.h"
#include <archive_entry.h>
#include <zlib.h>

using namespace storage_management;
using storage_test::require;
namespace fs = std::filesystem;
namespace {
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
  }
}
void interruptedArchiveWritesRecover() {
  const std::vector<std::pair<std::string, unsigned>> faults = {{"archive-entry-ready", 1}, {"archive-redo", 3},
      {"archive-write", 3}, {"archive-cursor", 3}, {"archive-before-truncate", 1}, {"archive-truncate", 1},
      {"archive-cleanup-ready", 1}, {"archive-manifest-unlinked", 1}, {"archive-cleanup-remove", 1}};
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
    require(calls == occurrence && interrupted.recoveryRequired, "injected archive write interruption retains recovery state");
    const auto recovered = RecoverStorageArchives(paths);
    if (recovered.recoveryRequired && !recovered.files.empty()) throw std::runtime_error(point + ": " + recovered.files.front().reason.detail);
    require(!recovered.failed && !recovered.recoveryRequired, "durable archive recovery finishes without encoding again");
    const auto files = readArchive(archive);
    require(files.at("one/a.flac") == std::string(1000, 'f') && files.at("two/image.png") == image &&
        files.at("one/chart.bms") == "#WAV01 a.wav", "recovery preserves completed media and overlapping copied payloads");
    require(fs::is_empty(paths.journalRoot), "successful archive recovery removes its owned journal");
    require(RecoverStorageArchives(paths).changedRoots.empty(), "archive recovery is idempotent");
  }
}
void writeSparseZip64(const fs::path &path, std::uint64_t offset = 0x100000000ULL + 4096,
    const std::string &audioName = "\x93\xfa\x96\x7b\x8c\xea.wav") {
  using storage_management::archive_detail::put;
  std::ofstream file(path, std::ios::binary);
  file.seekp(offset); std::string central;
  for (const auto &[name, payload] : std::vector<std::pair<std::string, std::string>>{
      {"movie.avi", std::string(1000, 'v')}, {audioName, std::string(10000, 'a')}, {"chart.bms", "fixture"}}) {
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
  put(end64, 24, 3, 8); put(end64, 32, 3, 8); put(end64, 40, central.size(), 8); put(end64, 48, offset, 8);
  std::string locator(20, '\0'); put(locator, 0, 0x07064b50, 4); put(locator, 8, offset + central.size(), 8); put(locator, 16, 1, 4);
  std::string end(22, '\0'); put(end, 0, 0x06054b50, 4); put(end, 8, 3, 2); put(end, 10, 3, 2);
  put(end, 12, central.size(), 4); put(end, 16, 0xffffffff, 4); put(end, 20, 12, 2);
  const auto tail = end64 + locator + end + "archive-note"; file.write(tail.data(), tail.size()); require(file.good(), "write sparse ZIP64 fixture");
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
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  require(review.eligibleCount == 2 && review.archives.size() == 1, "ZIP64 offsets and CP932 names remain eligible");
  StorageArchiveDependencies dependencies{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
    std::ofstream(output, std::ios::binary) << std::string(1000, 'f'); return EncodeResult{FileOutcome::Converted, output};
  }};
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy, MakeStoragePlatformPaths(temp.root / "private", {temp.root}), dependencies);
  require(report.converted == 1 && report.deleted == 1 && !report.recoveryRequired && fs::file_size(path) < 5000,
      "ZIP64 sparse ranges compact without reading or staging the hole");
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
  writeArchive(path, {{"songs/tone.wav", wav}, {"songs/video.avi", std::string(1000, 'v')}, {"songs/chart.bms", "#WAV01 tone.wav"}, {"other/image.png", "image"}});
  std::vector<short> originalPcm; SF_INFO originalInfo{}; std::atomic_bool originalCancelled = false;
  require(decodeAudioToPCM(fspath_to_path_t(path / "songs/tone.wav"), originalPcm, originalInfo, originalCancelled), "decode original archive audio");
  OptimizationRequest request; request.policy = request.inventory.policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete};
  request.inventory.groups = {{"songs", path / "songs", false, false, StorageSourceKind::Archive}, {"overlap", path / "songs", false, false, StorageSourceKind::Archive}};
  request.inventory.scanScratchRoot = temp.root / "scan"; request.preflight = BuildStoragePreflight(request.inventory);
  const auto report = StorageOptimizer(DefaultOptimizerDependencies(MakeStoragePlatformPaths(temp.root / "private", {temp.root}))).Run(request);
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
    if (mode != "stale") {
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
        archive_detail::Statement q(db.value, "SELECT target,after FROM redo"); require(q.step(), "find overlapping redo");
        StorageArchiveIO file(path, {}); const auto bytes = q.bytes(1); file.Write(q.integer(0), std::string_view(bytes).substr(0, bytes.size() / 2)); file.Sync();
      }
    }
    std::string error; const auto before = HashStorageFile(path, {}, error); auto result = RecoverStorageArchives(paths);
    if (foreign) {
      require(result.recoveryRequired && HashStorageFile(path, {}, error) == before, "foreign source changes block recovery before another write");
      { StorageArchiveIO file(path, {}); file.Write(foreignOffset, "i"); file.Sync(); }
      result = RecoverStorageArchives(paths);
    }
    require(!result.recoveryRequired && !result.failed && readArchive(path).at("image.png") == std::string(700000, 'i'),
        "partial overlapping writes replay without losing future source bytes");
  }
}
}
void storageArchiveTests() {
  for (const auto &[name, run] : std::vector<std::pair<const char *, void(*)()>>{
      {"manifest", manifestsExtractEntries}, {"preflight", selectedArchiveFoldersAreEligible},
      {"sandbox", archiveReviewSeparatesPrivateAndLibraryAccess},
      {"update", archiveUpdatesBatchWithoutFullCopy}, {"recovery", interruptedArchiveWritesRecover},
      {"zip64", sparseZip64AndLegacyNames}, {"invalid-encoding", malformedLegacyNamesAreRejected},
      {"playback", realArchiveOptimizerPreservesPlayback},
      {"boundaries", archivePolicyBoundaries}, {"rejections", unsafeAndStaleArchivesAreRejected},
      {"partial", partialWritesAndForeignChanges}}) {
    try { run(); } catch (const std::exception &error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
  }
}
