#include "../src/storage/StorageOptimizer.h"
#include "../src/storage/StorageArchive.h"
#include "../src/storage/StorageArchiveTransaction.h"
#include "../src/storage/StorageArchiveDb.h"
#include "../src/storage/StorageMeasurement.h"
#include "../src/storage/StorageMeasurementState.h"
#include "../src/ArchiveSourceIdentity.h"
#include "../src/audio/decoder.h"
#include "../src/ArchiveRAII.h"
#include "../src/ArchiveFile.h"
#include "StorageTestSupport.h"
#include <archive_entry.h>
#include <algorithm>
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
    archive_entry_set_filetype(entry, name.ends_with('/') ? AE_IFDIR : AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, contents.size());
    const bool written = archive_write_header(writer.get(), entry) == ARCHIVE_OK &&
        archive_write_data(writer.get(), contents.data(), contents.size()) == static_cast<la_ssize_t>(contents.size());
    archive_entry_free(entry);
    require(written, "write fixture ZIP entry");
  }
  require(archive_write_close(writer.get()) == ARCHIVE_OK, "finish fixture ZIP");
}
void archiveEncodersOverlapAndDrainSafely() {
  for (const int mode : {0, 1, 2}) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / "songs.zip";
    std::vector<std::pair<std::string, std::string>> files{
        {"a.mpg", std::string(1000, 'a')}, {"b.mpg", std::string(1000, 'b')}, {"c.wav", std::string(1000, 'c')}};
    for (unsigned i = 0; i < 9; ++i) files.emplace_back("later-" + std::to_string(i) + ".wav", std::string(1000, 'd'));
    writeArchive(path, files);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LossyVorbis, .bga = BgaAction::CompressH264}, .scanScratchRoot = paths.scanRoot};
    const auto review = BuildStoragePreflight(request);
    require(review.eligibleCount == 12, "parallel archive fixture exceeds the former eight-file staging window");
    std::mutex mutex; std::condition_variable changed;
    unsigned laterEncoded = 0; bool release = false, progressVisible = false;
    std::atomic<bool> cancelled = false;
    StorageArchiveDependencies deps{.encode = [&](const AssetCandidate &asset, const StoragePolicy &, const fs::path &output,
        Checkpoint check, ProgressCallback) {
      std::ifstream input(asset.source, std::ios::binary); const auto kind = input.get(); input.close();
      if (kind == 'a') { std::unique_lock lock(mutex); changed.wait(lock, [&] { return release; }); }
      if (!check()) return EncodeResult{FileOutcome::Cancelled, {}, {StorageReason::Cancelled, "cancel"}};
      { std::ofstream file(output); file << "tiny"; }
      if (kind != 'a') { std::lock_guard lock(mutex); ++laterEncoded; changed.notify_all(); }
      return EncodeResult{FileOutcome::Converted, output, {}};
    }, .faultPoint = [&](std::string_view point) {
      if (mode == 2 && point == "archive-entry-ready") throw std::runtime_error("interrupted prepared entry");
    }};
    auto run = std::async(std::launch::async, [&] {
      return UpdateStorageArchive(review.archives.front(), request.policy, paths, deps, [&] { return !cancelled; },
          [&](const StorageProgress &progress) {
            std::lock_guard lock(mutex);
            if (!release && progress.encodesDone == 11 && progress.encodesActive == 1 && progress.filesDone == 0)
              progressVisible = true;
            changed.notify_all();
          });
    });
    bool overlapped;
    { std::unique_lock lock(mutex);
      overlapped = changed.wait_for(lock, std::chrono::seconds(2), [&] { return laterEncoded == 11 && progressVisible; });
      cancelled = mode == 1; release = true; changed.notify_all(); }
    const auto report = run.get();
    require(overlapped, "all later archive encodes finish and publish activity while source-order commits wait for the first video");
    if (mode == 2) {
      require(report.recoveryRequired, "interrupted mutation retains its durable journal");
      const auto recovered = RecoverStorageArchives(paths);
      require(!recovered.failed && !recovered.recoveryRequired, "recovery recognizes and cleans speculative encoder staging");
    } else require(!report.failed && !report.recoveryRequired && report.cancelled == (mode == 1) &&
        report.converted == (mode == 1 ? 0 : 12), "parallel archive encoding preserves ordered commit and cancellation");
    const auto retained = readArchive(path);
    require(retained.size() == 12 && retained.at(mode == 1 ? "a.mpg" : "a.mp4") == (mode == 1 ? files[0].second : "tiny") &&
        retained.at(mode == 0 ? "b.mp4" : "b.mpg") == (mode == 0 ? "tiny" : files[1].second) &&
        retained.at(mode == 0 ? "c.ogg" : "c.wav") == (mode == 0 ? "tiny" : files[2].second),
        "only durable replacements survive cancellation or recovery");
    for (unsigned i = 0; i < 9; ++i)
      require(retained.at("later-" + std::to_string(i) + (mode == 0 ? ".ogg" : ".wav")) ==
          (mode == 0 ? "tiny" : std::string(1000, 'd')), "later staged files commit once or retain their original payloads");
    require(fs::is_empty(paths.journalRoot), "all archive workers join before staging cleanup");
  }
}
void independentArchivesAndDirectoriesShareEncodingCapacity() {
  storage_test::TempDirectory temp;
  const auto first = temp.root / "a.zip", second = temp.root / "b.zip";
  writeArchive(first, {{"a.mpg", std::string(1000, 'a')}});
  writeArchive(second, {{"b.mpg", std::string(1000, 'b')}});
  temp.write("c/c.wav", std::string(1000, 'c'));
  const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
  InventoryRequest input{.groups = {{"first", first, false, false, StorageSourceKind::Archive},
      {"second", second, false, false, StorageSourceKind::Archive}, {"local", temp.root / "c"}},
      .policy = {.audio = AudioAction::LossyVorbis, .bga = BgaAction::CompressH264}, .scanScratchRoot = paths.scanRoot};
  auto deps = DefaultOptimizerDependencies(paths); deps.executionLimits.audioWorkers = 1;
  deps.probeCodecs = [] { StorageCodecCapabilities caps; caps.vorbis.available = caps.x264.available = caps.mp4.available = true; return caps; };
  std::mutex mutex; std::condition_variable changed;
  bool release = false, firstEntered = false; unsigned laterEncoded = 0;
  deps.encodeAudio = deps.encodeVideo = [&](const AssetCandidate &asset, const StoragePolicy &, const fs::path &output, auto, auto) {
    std::ifstream source(asset.source); const auto kind = source.get(); source.close();
    if (kind == 'a') {
      std::unique_lock lock(mutex); firstEntered = true; changed.notify_all(); changed.wait(lock, [&] { return release; });
    }
    { std::ofstream file(output); file << "tiny"; }
    if (kind != 'a') { std::lock_guard lock(mutex); ++laterEncoded; changed.notify_all(); }
    return EncodeResult{FileOutcome::Converted, output, {}};
  };
  auto run = std::async(std::launch::async, [&] { return StorageOptimizer(deps).Run({.inventory = input, .policy = input.policy}); });
  bool overlapped;
  { std::unique_lock lock(mutex);
    overlapped = changed.wait_for(lock, std::chrono::seconds(2), [&] { return firstEntered && laterEncoded == 2; });
    release = true; changed.notify_all(); }
  const auto report = run.get();
  require(overlapped, "a held archive video does not block another archive or local directory");
  require(report.converted == 3 && !report.failed && !report.recoveryRequired && report.groupDeltas.size() == 3 &&
      readArchive(first).at("a.mp4") == "tiny" && readArchive(second).at("b.mp4") == "tiny" &&
      fs::file_size(temp.root / "c/c.ogg") == 4 && fs::is_empty(paths.journalRoot) && fs::is_empty(paths.temporaryRoot),
      "independent owners merge committed reports and drain their shared encoding budget");
}
void archiveReadAheadKeepsFullAudioCapacityWithVideos() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "mixed.zip";
  std::vector<std::pair<std::string, std::string>> files;
  for (unsigned i = 0; i < 3; ++i) files.emplace_back("song/video-" + std::to_string(i) + ".mpg", std::string(1000, 'v'));
  // Cross several metadata batches: images, already-compressed audio, other
  // files, and eligible media outside the selected subtree must not starve audio.
  for (unsigned i = 0; i < 2048; ++i) {
    const auto name = std::to_string(i);
    switch (i % 4) {
    case 0: files.emplace_back("song/image-" + name + ".png", "image"); break;
    case 1: files.emplace_back("song/kept-" + name + ".ogg", "audio"); break;
    case 2: files.emplace_back("song/readme-" + name + ".txt", "text"); break;
    case 3: files.emplace_back("unselected/audio-" + name + ".wav", "outside"); break;
    }
  }
  for (unsigned i = 0; i < 5; ++i) files.emplace_back("song/audio-" + std::to_string(i) + ".wav", std::string(1000, 'a'));
  writeArchive(path, files);
  const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
  InventoryRequest input{.groups = {{"song", path / "song", false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LossyVorbis, .bga = BgaAction::CompressH264}, .scanScratchRoot = paths.scanRoot};
  const auto review = BuildStoragePreflight(input);
  require(review.eligibleCount == 8, "only selected convertible media enters archive encoding");
  std::mutex mutex; std::condition_variable changed;
  unsigned audio = 0, video = 0, peakAudio = 0, peakVideo = 0;
  bool release = false;
  StorageArchiveDependencies deps{.encode = [&](const AssetCandidate &asset, const StoragePolicy &, const fs::path &output, auto, auto) {
    const bool isVideo = asset.kind == asobmshow::chart_assets::MediaKind::Video;
    { std::unique_lock lock(mutex);
      ++(isVideo ? video : audio);
      peakAudio = std::max(peakAudio, audio); peakVideo = std::max(peakVideo, video);
      changed.notify_all(); changed.wait(lock, [&] { return release; });
      --(isVideo ? video : audio); }
    std::ofstream(output) << "tiny";
    return EncodeResult{FileOutcome::Converted, output, {}};
  }, .executionLimits = {.audioWorkers = 4}};
  const auto started = std::chrono::steady_clock::now();
  auto run = std::async(std::launch::async, [&] { return UpdateStorageArchive(review.archives.front(), input.policy, paths, deps); });
  bool overlapped;
  { std::unique_lock lock(mutex);
    overlapped = changed.wait_for(lock, std::chrono::seconds(2), [&] { return audio == 4 && video == 2; });
    release = true; changed.notify_all(); }
  const auto readAheadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  const auto report = run.get();
  std::cout << "archive ineligible-gap dispatch ms=" << readAheadMs << '\n';
  require(overlapped && peakAudio == 4 && peakVideo == 2,
      "queued archive video does not reduce the full independent audio worker allocation");
  require(report.converted == 8 && !report.failed && !report.skipped && !report.recoveryRequired && fs::is_empty(paths.journalRoot),
      "bounded media read-ahead commits every queued target and cleans staging");
  const auto retained = readArchive(path);
  require(retained.size() == files.size(), "read-ahead preserves every archive entry");
  for (unsigned i = 0; i < 3; ++i) require(retained.at("song/video-" + std::to_string(i) + ".mp4") == "tiny", "video output is committed once");
  for (unsigned i = 0; i < 5; ++i) require(retained.at("song/audio-" + std::to_string(i) + ".ogg") == "tiny", "audio output is committed once");
  for (std::size_t i = 3; i < 2051; ++i)
    require(retained.at(files[i].first) == files[i].second, "read-ahead leaves skipped and unselected entries unchanged");
}
void archiveLookaheadRespectsAggregateScratch() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "bounded.zip";
  writeArchive(path, {{"a.wav", std::string(1000, 'a')}, {"b.mpg", std::string(1000, 'b')}, {"c.wav", std::string(1000, 'c')}});
  const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
  InventoryRequest input{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::CompressH264}, .scanScratchRoot = paths.scanRoot};
  const auto review = BuildStoragePreflight(input);
  std::mutex mutex; std::condition_variable changed;
  std::atomic<bool> entered = false;
  bool release = false, observed = false, bounded = false;
  StorageArchiveDependencies deps{.encode = [&](const AssetCandidate &asset, const StoragePolicy &, const fs::path &output, auto, auto) {
    std::ifstream source(asset.source); const auto kind = source.get(); source.close();
    if (kind == 'a') {
      entered = true;
      std::unique_lock lock(mutex);
      require(changed.wait_for(lock, std::chrono::seconds(2), [&] { return release; }), "owner continues progress while its scratch window is full");
    }
    std::ofstream(output) << "tiny";
    return EncodeResult{FileOutcome::Converted, output, {}};
  }, .scratchLimit = 8 * 1024 * 1024 + 3000};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  const auto report = UpdateStorageArchive(review.archives.front(), input.policy, paths, deps,
      [&] { return std::chrono::steady_clock::now() < deadline; }, [&](const StorageProgress &) {
    if (observed || !entered) return;
    unsigned inputs = 0;
    for (const auto &journal : fs::directory_iterator(paths.journalRoot))
      for (const auto &file : fs::directory_iterator(journal.path()))
        if (file.path().filename().string().starts_with("input-")) ++inputs;
    observed = true; bounded = inputs == 1;
    { std::lock_guard lock(mutex); release = true; changed.notify_all(); }
  });
  require(observed && bounded && report.converted == 3 && !report.failed && !report.recoveryRequired && fs::is_empty(paths.journalRoot),
      "aggregate scratch admits one staged entry at a time and reuses its reservation without skipping later files");
}
void archiveFailureStopsPeersBeforeJoiningEncoders() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "failure.zip";
  writeArchive(path, {{"a.mpg", std::string(1000, 'a')}, {"b.mpg", std::string(1000, 'b')}});
  const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
  InventoryRequest input{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::CompressH264}, .scanScratchRoot = paths.scanRoot};
  const auto review = BuildStoragePreflight(input);
  StorageWorkBudget budget({}, [] { return std::optional<std::uint64_t>(1024 * 1024 * 1024); });
  std::mutex mutex; std::condition_variable changed;
  bool pendingEntered = false, failed = false, cancelled = false, admissionStopped = false;
  StorageArchiveDependencies deps{.encode = [&](const AssetCandidate &asset, const StoragePolicy &, const fs::path &output,
      Checkpoint check, ProgressCallback) {
    std::ifstream source(asset.source); const auto kind = source.get(); source.close();
    if (kind == 'b') {
      std::unique_lock lock(mutex); pendingEntered = true; changed.notify_all();
      require(changed.wait_for(lock, std::chrono::seconds(2), [&] { return failed; }), "first archive entry reaches mutation failure");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (check() && std::chrono::steady_clock::now() < deadline) changed.wait_for(lock, std::chrono::milliseconds(5));
      cancelled = !check();
      bool waiting = false;
      admissionStopped = budget.RecoveryRequired() && !budget.TryAcquire(asobmshow::chart_assets::MediaKind::Audio, 1, waiting);
      return EncodeResult{FileOutcome::Cancelled, {}, {StorageReason::Cancelled, "cancel"}};
    }
    std::ofstream(output) << "tiny";
    return EncodeResult{FileOutcome::Converted, output, {}};
  }, .faultPoint = [&](std::string_view point) {
    if (point != "archive-entry-ready") return;
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, std::chrono::seconds(2), [&] { return pendingEntered; }), "speculative encoder is active at mutation failure");
    failed = true; changed.notify_all();
    throw std::runtime_error("interrupted archive mutation");
  }, .workBudget = &budget};
  const auto report = UpdateStorageArchive(review.archives.front(), input.policy, paths, deps, [&] { return !budget.RecoveryRequired(); });
  require(report.recoveryRequired && cancelled && admissionStopped,
      "archive failure cancels pending encoders and stops peer admission before joining them");
  const auto recovered = RecoverStorageArchives(paths);
  require(!recovered.failed && !recovered.recoveryRequired && fs::is_empty(paths.journalRoot), "failed parallel archive remains recoverable");
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
void deflatedPayloadRefillsAtExactOutputBoundary() {
  using storage_management::archive_detail::put;
  storage_test::TempDirectory temp; const auto path = temp.root / "boundary.zip";
  // A fixed block emits 14 zeroes, then stored blocks fill both 128 KiB
  // buffers exactly before the final stored block needs fresh input.
  const auto packed = std::string("\x62\x40\x01\x00\x00\xff\xff\x00\x00", 9) + std::string(65535, 'b') +
      std::string("\x00\xf3\xff\x0c\x00", 5) + std::string(65523, 'c') + std::string("\x01\x01\x00\xfe\xff", 5) + "x";
  const auto payload = std::string(14, '\0') + std::string(65535, 'b') + std::string(65523, 'c') + "x";
  const std::string name = "audio.wav";
  const auto crc = crc32(0, reinterpret_cast<const Bytef *>(payload.data()), payload.size());
  std::string local(30, '\0'); put(local, 0, 0x04034b50, 4); put(local, 4, 20, 2); put(local, 8, 8, 2);
  put(local, 14, crc, 4); put(local, 18, packed.size(), 4); put(local, 22, payload.size(), 4); put(local, 26, name.size(), 2);
  std::string central(46, '\0'); put(central, 0, 0x02014b50, 4); put(central, 4, 20, 2); put(central, 6, 20, 2);
  put(central, 10, 8, 2); put(central, 16, crc, 4); put(central, 20, packed.size(), 4); put(central, 24, payload.size(), 4);
  put(central, 28, name.size(), 2); central += name;
  std::string end(22, '\0'); put(end, 0, 0x06054b50, 4); put(end, 8, 1, 2); put(end, 10, 1, 2);
  put(end, 12, central.size(), 4); put(end, 16, local.size() + name.size() + packed.size(), 4);
  { std::ofstream file(path, std::ios::binary); file << local << name << packed << central << end; }
  StorageArchive archive(path, temp.root / "manifest.sqlite", {});
  const auto entry = archive.Next({}); require(entry.has_value(), "read deflate boundary entry");
  const auto output = temp.root / "decoded"; archive.Extract(*entry, output, {});
  std::ifstream file(output, std::ios::binary); const std::string decoded((std::istreambuf_iterator<char>(file)), {});
  require(decoded == payload, "valid deflate refills input after output ends exactly at a chunk boundary");
}
void existingArchiveDestinationsAreReused() {
  for (int variant = 0; variant < 3; ++variant) for (bool targetFirst : {false, true}) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / (variant == 2 ? "library.tar" : "library.zip");
    std::vector<std::pair<std::string, std::string>> files{{"prefix.wav", "redundant"}, {"prefix.flac", "valid prefix"}};
    const std::pair<std::string, std::string> source{"song.wav", "redundant source"}, target{"song.flac", "valid target retained byte for byte"};
    files.push_back(targetFirst ? target : source); files.push_back(targetFirst ? source : target);
    files.emplace_back("chart.bms", "#WAV01 song.wav");
    writeArchive(path, files, variant == 1, variant == 2);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    InventoryRequest request{.groups = {{"archive", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = paths.scanRoot};
    const auto review = BuildStoragePreflight(request);
    require(review.eligibleCount == 2 && review.archives.size() == 1, "archive preflight offers existing destinations for reuse");
    unsigned validations = 0;
    StorageArchiveDependencies deps{.validateExisting = [&](const AssetCandidate &asset, Checkpoint) {
      std::ifstream input(asset.destination, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(input)), {});
      require(bytes == "valid prefix" || bytes == target.second, "validate retained target at its current compacted position");
      ++validations; return StorageDiagnostic{};
    }};
    const auto result = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
    const auto retained = readArchive(path);
    require(!result.failed && !result.recoveryRequired && result.deleted == 2 && validations == 2 &&
        retained.size() == 3 && retained.at("song.flac") == target.second && retained.at("chart.bms") == files.back().second,
        "ZIP and TAR remove only redundant sources and retain destination bytes in either physical order");
  }
  for (bool invalid : {false, true}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "library.zip";
    const std::vector<std::pair<std::string, std::string>> files{{"song.wav", "source"}, {"song.ogg", "target"}};
    writeArchive(path, files);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    InventoryRequest request{.groups = {{"archive", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LossyVorbis}, .scanScratchRoot = paths.scanRoot};
    const auto review = BuildStoragePreflight(request);
    require(review.archives.size() == 1 && review.eligibleCount == 1, "existing Vorbis is a potential archive target");
    StorageArchiveDependencies deps{.validateExisting = [&](const auto &, Checkpoint) {
      return StorageDiagnostic{invalid ? StorageReason::ValidationFailed : StorageReason::Cancelled, "preserve both files"};
    }};
    const auto result = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
    require(!result.deleted && readArchive(path).size() == 2, "invalid or cancelled destination validation never removes archived source");
  }
}
void archiveReuseRequiresExactParentAndStem() {
  for (bool tar : {false, true}) for (const auto targetName : {"song/a.flac", "Song/A.flac", "Song/a.FLAC"}) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / (tar ? "library.tar" : "library.zip");
    const bool extensionOnly = std::string_view(targetName) == "Song/a.FLAC";
    const std::vector<std::pair<std::string, std::string>> files{
        {"Song/a.wav", "selected recording"}, {targetName, "existing recording"}};
    writeArchive(path, files, false, tar);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    InventoryRequest request{.groups = {{"selected", path / "Song", false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = paths.scanRoot};
    const auto review = BuildStoragePreflight(request);
    require(review.complete && review.archives.size() == 1, "review case-distinct archive alternatives");
    unsigned validations = 0;
    std::atomic<unsigned> encodes = 0;
    StorageArchiveDependencies deps{.encode = [&](const auto &, const auto &, const auto &, auto, auto) {
      ++encodes; return EncodeResult{};
    }, .validateExisting = [&](const AssetCandidate &, Checkpoint) {
      ++validations; return StorageDiagnostic{};
    }};
    // Exercise Apply directly even when review has excluded the candidate, so
    // its physical manifest must enforce the same spelling rule independently.
    const auto result = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
    const auto retained = readArchive(path);
    require(!result.failed && !result.recoveryRequired && !result.converted && !encodes && fs::is_empty(paths.journalRoot),
        "archive case checks settle without encoding or recovery");
    if (extensionOnly) {
      require(review.eligibleCount == 1 && result.deleted == 1 && validations == 1 &&
          retained == std::map<std::string, std::string>{{"Song/a.FLAC", "existing recording"}},
          "extension-only case differences still reuse the exact parent and stem");
    } else {
      require(!result.deleted && !validations && retained == std::map<std::string, std::string>(files.begin(), files.end()),
          "case-distinct archive parents or stems never validate an unrelated target or delete the source");
      require(review.eligibleCount == 0 && review.skippedCount == 1 && review.skips.size() == 1 &&
          review.skips.front().reason.code == StorageReason::Collision,
          "review excludes case-distinct archive destinations as collisions");
    }
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
void archiveFolderSelectionPreservesCase() {
  storage_test::TempDirectory temp;
  const auto archive = temp.root / "library.zip";
  writeArchive(archive, {{"Song/selected.avi", std::string(20, 'a')},
      {"song/retained.avi", std::string(30, 'b')}});
#ifdef _WIN32
  fs::path resolved, inner;
  require(FindStorageArchive(archive / "Song", resolved, inner, {temp.root / "LIBRARY.ZIP"}) &&
              resolved == archive && inner == "Song",
          "archive file grants with different case preserve the requested native prefix and inner spelling");
#endif
  InventoryRequest request{.groups = {{"upper", archive / "Song", false, false, StorageSourceKind::Archive},
      {"lower", archive / "song", false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::Delete}};
  const auto measured = MeasureStorageGroups(request);
  require(measured.groups.size() == 2 && measured.groups[0].totalBytes == 20 && measured.groups[1].totalBytes == 30,
          "case-distinct archive folders have independent measured totals on every host");
  request.groups.resize(1);
  const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
  request.scanScratchRoot = paths.scanRoot;
  const auto review = BuildStoragePreflight(request);
  require(review.complete && review.archives.size() == 1 && review.eligibleCount == 1 && review.eligibleBytes == 20,
          "archive review selects only the exact-cased inner folder");
  StorageArchiveDependencies dependencies;
  dependencies.measurementBaselines = measured.groups;
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy, paths, dependencies);
  require(report.deleted == 1 && !report.failed && !report.recoveryRequired &&
              readArchive(archive) == std::map<std::string, std::string>{{"song/retained.avi", std::string(30, 'b')}},
          "archive apply never deletes a differently cased sibling folder");
  for (const auto &delta : report.groupDeltas)
    if (delta.groupId == "lower") require(delta.bytesBefore == 0 && delta.bytesAfter == 0,
        "archive delta aggregation leaves the case-distinct sibling unchanged");
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
  std::vector<std::pair<std::string, std::string>> files = {{"a.wav", std::string(1000, 'a')}, {"b.wav", std::string(1000, 'b')}};
  for (int i = 0; i != 51; ++i) {
    const auto stem = "conflict-" + std::to_string(i);
    files.emplace_back(stem + ".wav", "wav"); files.emplace_back(stem + ".flac", "flac");
  }
  writeArchive(path, files);
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
      .policy = {.audio = AudioAction::LossyVorbis}, .scanScratchRoot = temp.root / "scan"};
  StorageScanMetrics metrics;
  const auto review = BuildStoragePreflight(request, {}, &metrics);
  require(review.complete && review.eligibleCount == 2 && review.skippedCount == 102 &&
      metrics.archiveIndexVisits == 1 && metrics.archiveEntries == 104,
      "review consumes one cached index without rebuilding physical layout metadata");
  std::string error; const auto before = HashStorageFile(path, {}, error);
  std::atomic<unsigned> encodes = 0;
  auto deps = DefaultOptimizerDependencies(MakeStoragePlatformPaths(temp.root / "private", {temp.root}));
  deps.encodeAudio = [&](const auto &, const auto &, const auto &, auto, auto) {
    ++encodes; return EncodeResult{};
  };
  const auto result = StorageOptimizer(deps).Run({.inventory = request, .policy = request.policy, .preflight = review});
  require(result.failed == 1 && !result.recoveryRequired && encodes == 0 && HashStorageFile(path, {}, error) == before,
      "Apply validates physical layout before encoding or writing despite a cached review");
  require(result.skipped == 102 && result.files.size() == 100 &&
      result.files.front().outcome == FileOutcome::Failed && result.files.front().source == path &&
      !result.files.front().reason.detail.empty(),
      "archive rejection remains the first diagnostic when review skips fill the bounded report");
}
void damagedPayloadDoesNotAbortOtherSelectedSongs() {
  for (int mode = 0; mode != 6; ++mode) {
    storage_test::TempDirectory temp; const auto path = temp.root / "songs.zip";
    std::vector<std::pair<std::string, std::string>> files = {
        {"bad/audio.wav", std::string(1000, 'b')}, {"good/audio.wav", std::string(1000, 'g')}};
    if (mode % 3 == 2) std::reverse(files.begin(), files.end());
    writeArchive(path, files, mode >= 3);
    std::string preserved;
    std::uint64_t damagedOffset = 0;
    {
      StorageArchive archive(path, temp.root / "before.sqlite", {});
      std::optional<std::uint64_t> cursor;
      while (auto entry = archive.Next(cursor)) {
        cursor = entry->offset;
        if (entry->name != "bad/audio.wav") continue;
        preserved = archive.io.Read(entry->offset, entry->span);
        preserved[entry->dataOffset - entry->offset] = '\x07'; damagedOffset = entry->dataOffset;
      }
    }
    require(!preserved.empty(), "locate payload checksum fixture");
    { std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out); file.seekp(damagedOffset); file.put('\x07'); }
    const bool selectAll = mode % 3 != 0;
    InventoryRequest request{.groups = {{"selection", selectAll ? path : path / "good", false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    require(review.eligibleCount == (selectAll ? 2 : 1), "review selects either all songs or just the unaffected song");
    std::atomic<unsigned> encodes = 0;
    StorageArchiveDependencies deps{.encode = [&](const auto &, const auto &, const auto &output, auto, auto) {
      ++encodes; std::ofstream(output, std::ios::binary) << "flac";
      return EncodeResult{FileOutcome::Converted, output};
    }};
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    const auto result = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
    require(result.converted == 1 && result.failed == (selectAll ? 1 : 0) && !result.recoveryRequired &&
        encodes == 1 && fs::is_empty(paths.journalRoot),
        "one damaged payload cannot abort other selected songs before or after a committed conversion");
    if (selectAll) require(result.files.front().source == path / "bad/audio.wav" &&
        result.files.front().reason.code == StorageReason::ValidationFailed,
        "failed payload identifies the affected song instead of reporting only the container");
    StorageArchive after(path, temp.root / "after.sqlite", {});
    std::optional<std::uint64_t> cursor;
    bool retained = false, converted = false;
    while (auto entry = after.Next(cursor)) {
      cursor = entry->offset;
      if (entry->name == "bad/audio.wav") retained = after.io.Read(entry->offset, entry->span) == preserved;
      if (entry->name == "good/audio.flac") converted = true;
    }
    require(retained && converted, "failed payload's original record is preserved exactly beside the successful conversion");
  }
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
void explicitArchiveDirectoriesSurviveRepeatedUpdates() {
  for (bool recover : {false, true}) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / "directories.zip";
    writeArchive(path, {{"song/", ""}, {"song/audio.wav", std::string(2000, 'a')},
        {"other.wav", std::string(2000, 'b')}, {"song/chart.bms", "chart"}});
    InventoryRequest request{.groups = {{"song", path / "song", false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    require(review.complete && review.eligibleCount == 1 && review.archives.size() == 1,
        "explicit ZIP directories permit review of one selected song");
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    StorageArchiveDependencies deps{.encode = [](const auto &, const auto &, const auto &output, auto, auto) {
      std::ofstream(output, std::ios::binary) << "converted";
      return EncodeResult{FileOutcome::Converted, output};
    }, .faultPoint = [&](std::string_view point) {
      if (recover && point == "archive-entry-ready") throw std::runtime_error("interrupt directory archive update");
    }};
    const auto result = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
    if (recover) {
      require(result.recoveryRequired, "explicit directories allow conversion to reach the durable recovery point");
      const auto recovered = RecoverStorageArchives(paths);
      require(!recovered.failed && !recovered.recoveryRequired && recovered.converted == 1,
          "recover conversion in a ZIP with explicit directory entries");
    } else require(!result.failed && !result.recoveryRequired && result.converted == 1,
        "explicit directory spelling matches the physical manifest during Apply");
    auto files = readArchive(path);
    require(files.at("song/audio.flac") == "converted" && files.at("other.wav") == std::string(2000, 'b') &&
        files.at("song/chart.bms") == "chart", "first update preserves unselected audio and chart contents");
    request.groups.front().path = path;
    const auto nextReview = BuildStoragePreflight(request);
    require(nextReview.complete && nextReview.eligibleCount == 1 && nextReview.archives.size() == 1,
        "refreshed browsing index selects the remaining audio after conversion or recovery");
    deps.faultPoint = {};
    const auto nextResult = UpdateStorageArchive(nextReview.archives.front(), request.policy, paths, deps);
    require(!nextResult.failed && !nextResult.recoveryRequired && nextResult.converted == 1,
        "whole-archive Apply still matches explicit directories after the first update");
    files = readArchive(path);
    require(files.at("song/audio.flac") == "converted" && files.at("other.flac") == "converted" &&
        files.at("song/chart.bms") == "chart", "repeated updates preserve both converted payloads and the chart");
    StorageArchive checked(path, temp.root / "checked.sqlite", {});
    const auto first = checked.Next();
    require(first && first->directory && first->rawName == "song/" && first->name == "song",
        "updates retain the original explicit directory record");
  }
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
  unsigned advances = 0; StorageArchiveMetrics metrics;
  StorageArchiveDependencies deps{.faultPoint = [&](std::string_view point) {
    if (point == "archive-cursor") ++advances;
  }, .metrics = &metrics};
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
  require(report.deleted == 1 && !report.failed && !report.recoveryRequired, "batched compaction completes");
  require(advances < 4, "small retained records share durable cursor advances instead of syncing each file");
  require(metrics.sourceWriteRanges <= 2, "adjacent retained records share one write range before the trailer");
  const auto actual = readArchive(path);
  require(actual.size() == 4, "only the selected video is deleted");
  for (std::size_t i = 1; i < files.size(); ++i)
    require(actual.at(files[i].first) == files[i].second, "batched compaction preserves each retained payload");
}
void unchangedArchivePrefixAvoidsFrequentCheckpoints() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "prefix.zip";
  std::vector<std::pair<std::string, std::string>> files;
  for (unsigned i = 0; i < 512; ++i) files.emplace_back(std::to_string(i) + ".png", "image");
  files.emplace_back("movie.avi", "video"); writeArchive(path, files);
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  StorageArchiveMetrics metrics;
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), {.metrics = &metrics});
  require(report.deleted == 1 && !report.failed && metrics.durableBatches <= 3,
      "unchanged prefix metadata does not force frequent durable checkpoints");
  const auto actual = readArchive(path);
  require(actual.size() == 512 && !actual.contains("movie.avi"), "prefix batching deletes only the selected video");
  for (unsigned i = 0; i < 512; ++i) require(actual.at(std::to_string(i) + ".png") == "image", "prefix batching preserves each image");
}
void smallArchiveRecordsRecoverAcrossBatches() {
  for (const auto point : {"archive-redo", "archive-source-synced"}) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / "small-records.zip";
    std::vector<std::pair<std::string, std::string>> files = {{"movie.avi", "video"}};
    for (unsigned i = 0; i < 1100; ++i) files.emplace_back(std::to_string(i) + ".png", "image" + std::to_string(i));
    writeArchive(path, files);
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    StorageArchiveDependencies deps{.faultPoint = [&](std::string_view name) {
      if (name == point) throw std::runtime_error("interrupt a coalesced small-record batch");
    }};
    require(UpdateStorageArchive(review.archives.front(), request.policy, paths, deps).recoveryRequired,
        "small-record batch reaches a durable interruption point");
    const auto recovered = RecoverStorageArchives(paths);
    require(!recovered.failed && !recovered.recoveryRequired && recovered.deleted == 1,
        "recovery accepts bounded redo with many small records");
    const auto actual = readArchive(path);
    require(actual.size() == 1100 && !actual.contains("movie.avi"), "coalesced recovery removes only the selected video");
    for (unsigned i = 0; i < 1100; ++i)
      require(actual.at(std::to_string(i) + ".png") == "image" + std::to_string(i), "coalesced recovery preserves each distinct payload");
  }
}
void preparedArchiveHashFailurePrecedesWrites() {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "prepared.zip";
  writeArchive(path, {{"audio.wav", std::string(3 * 1024 * 1024, 'a')}, {"chart.bms", "chart"}});
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
  std::string error; const auto original = HashStorageFile(path, {}, error);
  fs::path replacement;
  StorageArchiveMetrics metrics;
  const std::string encoded(2 * 1024 * 1024, 'f');
  StorageArchiveDependencies deps{.encode = [&](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
    std::ofstream file(output, std::ios::binary); file << encoded; file.close();
    return EncodeResult{.outcome = FileOutcome::Converted, .outputPath = output};
  }, .faultPoint = [&](std::string_view name) {
    if (name != "archive-entry-ready") return;
    for (const auto &entry : fs::directory_iterator(paths.journalRoot)) replacement = entry.path() / "replacement.bin";
    StorageArchiveIO file(replacement, {}); file.Write(1024 * 1024 + 123, "X"); file.Sync();
  }, .metrics = &metrics};
  const auto result = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
  require(result.recoveryRequired && metrics.sourceWriteRanges == 0 && HashStorageFile(path, {}, error) == original,
      "a corrupt prepared chunk fails the whole hash batch before any archive write");
  const auto failedRecovery = RecoverStorageArchives(paths);
  require(failedRecovery.recoveryRequired && HashStorageFile(path, {}, error) == original,
      "recovery also refuses the changed prepared payload before writing");
  { StorageArchiveIO file(replacement, {}); file.Write(1024 * 1024 + 123, "f"); file.Sync(); }
  const auto recovered = RecoverStorageArchives(paths);
  require(!recovered.failed && !recovered.recoveryRequired && recovered.converted == 1,
      "restoring prepared bytes permits recovery without re-encoding");
  const auto actual = readArchive(path);
  require(actual.size() == 2 && actual.at("audio.flac") == encoded && actual.at("chart.bms") == "chart",
      "recovered hash batch installs exact prepared bytes and preserves the next entry");
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
  for (bool wide : {false, true}) for (bool signedDescriptor : {false, true}) for (int damage = 0; damage < 5; ++damage) {
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
      if ((damage == 3 && name == "movie.avi") || (damage == 4 && name == "chart.bms"))
        contents += "padding or extra archive data";
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
          "invalid descriptors and unmodeled ZIP gaps reject Apply before archive mutation");
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
void archiveChangesUpdateCachedInnerMeasurements() {
  for (bool tar : {false, true}) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / (tar ? "library.tar" : "library.zip");
    std::vector<std::pair<std::string, std::string>> files{
        {"chosen/sound.wav", std::string(1000, 'a')}, {"chosen/art.png", std::string(20, 'i')},
        {"chosen/chart.bms", "chart"}, {"sibling/movie.mp4", std::string(50, 'v')},
        {"sibling/sound.flac", std::string(30, 'f')}, {"sibling/chart.bms", "chart"}};
    for (unsigned i = 0; i < 101; ++i) files.emplace_back("chosen/nested/" + std::to_string(i) + ".avi", std::string(10, 'v'));
    writeArchive(path, files, false, tar);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    InventoryRequest displayed{.groups = {{"all", path, false, false, StorageSourceKind::Archive},
        {"chosen", path / "chosen", false, false, StorageSourceKind::Archive},
        {"nested", path / "chosen/nested", false, false, StorageSourceKind::Archive},
        {"sibling", path / "sibling", false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete}, .scanScratchRoot = paths.scanRoot};
    auto measured = MeasureStorageGroups(displayed);
    require(measured.complete && measured.groups.size() == 4, "measure displayed archive groups before mutation");
    StorageMeasurementRows rows;
    for (auto &row : measured.groups) { row.generation = 7; rows.emplace(row.groupId, row); }
    const auto previousIdentity = rows.at("all").sourceIdentity;
    auto selected = displayed; selected.groups = {displayed.groups[1]};
    const auto review = BuildStoragePreflight(selected);
    auto dependencies = DefaultOptimizerDependencies(paths);
    dependencies.encodeAudio = [](const auto &, const auto &, const fs::path &output, auto, auto) {
      std::ofstream(output, std::ios::binary) << std::string(100, 'f');
      return EncodeResult{FileOutcome::Converted, output};
    };
    const auto report = StorageOptimizer(dependencies).Run({.inventory = selected, .policy = selected.policy,
        .preflight = review, .measurementBaselines = measured.groups});
    require(!report.failed && !report.recoveryRequired && report.converted == 1 && report.deleted == 101,
        "archive completes conversion and more deletions than the retained detail limit");
    require(ApplyStorageReportMeasurements(rows, displayed.groups, report).empty(),
        "successful archive changes update all displayed inner totals without requesting another index measurement");
    require(rows.at("all").totalBytes == 210 && rows.at("all").audioBytes == 130 &&
        rows.at("all").videoBytes == 50 && rows.at("all").imageBytes == 20,
        "archive ancestor totals include every committed change beyond the detail limit");
    require(rows.at("chosen").totalBytes == 125 && rows.at("chosen").audioBytes == 100 &&
        rows.at("chosen").videoBytes == 0 && rows.at("chosen").imageBytes == 20 &&
        rows.at("nested").totalBytes == 0 && rows.at("nested").videoBytes == 0,
        "selected and nested totals receive exact logical media deltas");
    require(rows.at("sibling").totalBytes == 85 && rows.at("sibling").audioBytes == 30 &&
        rows.at("sibling").videoBytes == 50, "unaffected archive sibling totals remain available");
    const auto nextIdentity = archive_source_identity::KeyForPath(path);
    require(!nextIdentity.empty() && nextIdentity != previousIdentity, "archive source identity advances after commit");
    for (const auto &[id, row] : rows)
      require(row.sourceIdentity == nextIdentity && row.generation == 7,
          "every retained archive row advances to the committed identity without changing its measured generation");
  }
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
void growingFlacArchiveRecovery() {
  for (bool tar : {false, true}) {
    for (const std::string point : {"", "archive-entry-ready", "archive-growth-synced", "archive-redo",
        "archive-source-synced", "archive-replacement-appended", "archive-relocations-synced", "archive-truncate-synced"}) {
      storage_test::TempDirectory temp;
      const auto path = temp.root / (tar ? "growing.tar" : "growing.zip");
      writeArchive(path, {{"first.wav", "first"}, {"image.png", std::string(3000, 'i')},
          {"middle.wav", "middle"}, {"movie.avi", "video"}, {"chart.bms", "#WAV01 first.wav"},
          {"last.wav", "last"}}, !tar, tar);
      const auto before = fs::file_size(path);
      InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
          .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
      const auto review = BuildStoragePreflight(request);
      const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
      StorageArchiveMetrics metrics;
      StorageArchiveDependencies deps{.encode = [](const auto &, const auto &, const fs::path &output, auto, auto) {
        std::ofstream(output, std::ios::binary) << std::string(12000, 'f');
        return EncodeResult{FileOutcome::Converted, output};
      }, .faultPoint = [&](std::string_view name) {
        if (!point.empty() && name == point) throw std::runtime_error("interrupt growing archive");
      }, .metrics = &metrics};
      auto report = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
      if (!point.empty()) {
        require(report.recoveryRequired, "growing archive reaches its durable interruption point");
        report = RecoverStorageArchives(paths);
      }
      if (report.recoveryRequired || report.failed) throw std::runtime_error(point + ": " + report.files.front().reason.detail);
      const auto files = readArchive(path);
      require(files.at("first.flac") == std::string(12000, 'f') && files.at("image.png") == std::string(3000, 'i') &&
          files.at("chart.bms") == "#WAV01 first.wav", "growing FLAC recovery preserves replacement and retained records");
      if (point.empty() || point == "archive-relocations-synced" || point == "archive-truncate-synced")
        require(report.converted == 3 && report.deleted == 1 && files.at("middle.flac") == std::string(12000, 'f') &&
            files.at("last.flac") == std::string(12000, 'f'), "all first, middle and last growing replacements survive compaction");
      require(fs::is_empty(paths.journalRoot), "growing archive recovery retires its journal");
      require(report.reclaimedBytes == 0 && fs::file_size(path) > before &&
          report.addedBytes == fs::file_size(path) - before,
          "larger archive does not underflow reclaimed bytes");
      require(metrics.peakScratchBytes < 50000, "lookahead retains at most three 12 KB outputs and one replacement");
    }
  }
}
void growingArchiveMultiBatchRecovery() {
  const std::string payload(9 * 1024 * 1024 + 17, 'f');
  for (bool stopDuringRelocation : {false, true}) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / "growing.zip";
    writeArchive(path, {{"chart.bms", "chart"}, {"a.wav", "audio"}});
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    bool appended = false;
    StorageArchiveDependencies deps{.encode = [&](const auto &, const auto &, const fs::path &output, auto, auto) {
      std::ofstream(output, std::ios::binary) << payload;
      return EncodeResult{FileOutcome::Converted, output};
    }, .faultPoint = [&](std::string_view point) {
      if (point == "archive-replacement-appended") appended = true;
      if (point == "archive-source-synced" && appended == stopDuringRelocation)
        throw std::runtime_error("interrupt within a multi-batch replacement");
    }};
    require(UpdateStorageArchive(review.archives.front(), request.policy, paths, deps).recoveryRequired,
        "interrupt during append or relocation before the entire record completes");
    const auto recovered = RecoverStorageArchives(paths);
    require(!recovered.failed && !recovered.recoveryRequired && recovered.converted == 1 &&
        readArchive(path).at("a.flac") == payload && readArchive(path).at("chart.bms") == "chart",
        "multi-batch growth resumes with exact replacement and retained contents");
  }
}
void interruptedArchiveWritesRecover() {
  const std::vector<std::pair<std::string, unsigned>> faults = {{"archive-entry-ready", 1}, {"archive-redo", 2},
      {"archive-write", 1}, {"archive-source-synced", 1}, {"archive-cursor", 2}, {"archive-before-truncate", 1}, {"archive-truncate", 1},
      {"archive-truncate-synced", 1}, {"archive-cleanup-ready", 1}, {"archive-manifest-unlinked", 1}, {"archive-cleanup-remove", 1}};
  for (const auto &[point, occurrence] : faults) {
    storage_test::TempDirectory temp;
    const auto archive = temp.root / "library.zip";
    std::string image(point == "archive-entry-ready" ? 10 * 1024 * 1024 : 700000, 'i');
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
    if (point == "archive-entry-ready") {
      for (const bool afterBatch : {false, true}) {
        bool quit = false;
        std::uint64_t stoppedAt = 0;
        const auto cancelled = RecoverStorageArchives(paths, [&] { return !quit; }, {},
            [&](const StorageRecoveryProgress &progress) {
              stoppedAt = progress.archiveBytesDone;
              if (afterBatch ? progress.archiveBytesDone > 0 :
                  progress.phase == StorageRecoveryPhase::CheckingArchive) quit = true;
            });
        require(quit && cancelled.cancelled && !fs::is_empty(paths.journalRoot),
            "quit during archive recovery retains a resumable journal");
        if (afterBatch) require(stoppedAt > 0 && stoppedAt < image.size(),
            "quit stops at a durable batch within a large archive entry");
        if (!afterBatch) require(HashStorageFile(archive, {}, error) == beforeRecovery,
            "quit before archive validation leaves source bytes unchanged");
      }
    }
    bool observed = false, completed = false, finalizing = false, validCompletion = true;
    const auto recovered = RecoverStorageArchives(paths, {}, {}, [&](const StorageRecoveryProgress &progress) {
      observed = true;
      if (progress.phase == StorageRecoveryPhase::FinalizingArchive) finalizing = true;
      if (progress.phase == StorageRecoveryPhase::Complete) {
        completed = true;
        validCompletion &= progress.completedOperations == 1 && progress.currentPath == archive &&
            fs::is_empty(paths.journalRoot);
      }
      throw std::runtime_error("injected progress display failure");
    });
    require(observed, "interrupted archive recovery publishes a visible phase");
    if (point == "archive-write" || point == "archive-truncate") {
      require(recovered.recoveryRequired && !completed && HashStorageFile(archive, {}, error) == beforeRecovery,
          "ambiguous source writes retain recovery evidence without another mutation");
      continue;
    }
    if (recovered.recoveryRequired && !recovered.files.empty()) throw std::runtime_error(point + ": " + recovered.files.front().reason.detail);
    require(!recovered.failed && !recovered.recoveryRequired, "durable archive recovery finishes without encoding again");
    require(finalizing && validCompletion && (point.starts_with("archive-cleanup-") ||
        point == "archive-manifest-unlinked" || completed),
        "progress reaches finalization and counts completed archive operations only after durable cleanup");
    const auto files = readArchive(archive);
    require(files.at("one/a.flac") == std::string(1000, 'f') && files.at("two/image.png") == image &&
        files.at("one/chart.bms") == "#WAV01 a.wav", "recovery preserves completed media and overlapping copied payloads");
    require(fs::is_empty(paths.journalRoot), "successful archive recovery removes its owned journal");
    require(RecoverStorageArchives(paths).changedRoots.empty(), "archive recovery is idempotent");
  }
}
void archiveRecoveryAfterContainerRelocation() {
  for (const std::string_view scenario : {"relocated", "foreign", "traversal"}) {
    storage_test::TempDirectory temp;
    const auto oldRoot = temp.root / "old-container", newRoot = temp.root / "new-container";
    fs::create_directories(oldRoot / "Documents");
    const auto archive = oldRoot / "Documents/library.zip";
    writeArchive(archive, {{"movie.avi", std::string(1000, 'v')}, {"chart.bms", "unchanged chart"}});
    auto paths = MakeStoragePlatformPaths(oldRoot / "Library", {oldRoot / "Documents"}, true);
    InventoryRequest request{.groups = {{"archive", archive, false, false, StorageSourceKind::Archive}},
        .policy = {.bga = BgaAction::Delete}, .pathAccessRoots = {oldRoot / "Documents"},
        .scanScratchRoot = paths.scanRoot};
    const auto review = BuildStoragePreflight(request);
    require(review.archives.size() == 1, "review relocation archive fixture");
    StorageArchiveDependencies dependencies{.faultPoint = [](std::string_view point) {
      if (point == "archive-redo") throw std::runtime_error("interrupt before archive write");
    }};
    require(UpdateStorageArchive(review.archives.front(), request.policy, paths, dependencies).recoveryRequired,
        "relocation fixture retains a durable archive redo record");
    if (scenario == "traversal") {
      const auto journal = fs::directory_iterator(paths.journalRoot)->path();
      archive_detail::Database db(journal / "manifest.sqlite", false);
      archive_detail::Statement update(db.value, "UPDATE control SET path=? WHERE id=1");
      update.text(1, (oldRoot / "Documents/../Documents/library.zip").string()); update.step();
    }
    fs::rename(oldRoot, newRoot);
    const auto relocated = newRoot / "Documents/library.zip";
    paths = MakeStoragePlatformPaths(newRoot / "Library", {newRoot / "Documents"}, true);
    paths.resolveRecoveryPath = [=](const fs::path &path) {
      return IsWithinFolder(path, oldRoot / "Documents") ?
          (newRoot / path.lexically_relative(oldRoot)).lexically_normal() : path;
    };
    if (scenario == "foreign") {
      fs::rename(relocated, newRoot / "Documents/original.zip");
      writeArchive(relocated, {{"foreign.bms", "foreign chart"}});
    }
    std::string error; const auto before = HashStorageFile(relocated, {}, error);
    const auto recovered = RecoverStorageArchives(paths);
    if (scenario != "relocated") {
      require(recovered.recoveryRequired && HashStorageFile(relocated, {}, error) == before &&
          !fs::is_empty(paths.journalRoot), "relocated archive recovery rejects foreign inodes and parent traversal");
    } else {
      require(!recovered.recoveryRequired && fs::is_empty(paths.journalRoot),
          "archive recovery resolves source paths after the containing directory moves");
      const auto files = readArchive(relocated);
      require(files.size() == 1 && files.at("chart.bms") == "unchanged chart",
          "relocated archive recovery finishes deletion without changing retained chart bytes");
    }
  }
}
void writeSparseZip64(const fs::path &path, std::uint64_t offset = 128ULL * 1024 * 1024 * 1024 + 4096,
    const std::string &audioName = "\x93\xfa\x96\x7b\x8c\xea.wav", bool prefix = false,
    const std::string &unicodeName = {}, bool unicodeCentral = true, bool zip64 = true,
    const std::string &audioLocalExtra = {}, const std::string &audioCentralExtra = {},
    const std::string &firstCentralExtra = {}) {
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
    std::string unicode;
    if (name == audioName && !unicodeName.empty()) {
      unicode.resize(9, '\0'); put(unicode, 0, 0x7075, 2); put(unicode, 2, 5 + unicodeName.size(), 2);
      put(unicode, 4, 1, 1); put(unicode, 5, crc32(0, reinterpret_cast<const Bytef *>(name.data()), name.size()), 4);
      unicode += unicodeName;
    }
    const auto localExtra = unicode + (name == audioName ? audioLocalExtra : std::string{});
    put(local, 28, localExtra.size(), 2);
    const auto record = local + name + localExtra + payload; file.write(record.data(), record.size());
    std::string row(46, '\0'); put(row, 0, 0x02014b50, 4); put(row, 4, 45, 2); put(row, 6, 45, 2); put(row, 16, crc, 4);
    put(row, 20, payload.size(), 4); put(row, 24, payload.size(), 4); put(row, 28, name.size(), 2); put(row, 30, 12, 2);
    put(row, 32, 10, 2); put(row, 42, zip64 ? 0xffffffff : offset, 4);
    std::string extra;
    if (zip64) { extra.resize(12, '\0'); put(extra, 0, 1, 2); put(extra, 2, 8, 2); put(extra, 4, offset, 8); }
    if (unicodeCentral) extra += unicode;
    if (name == audioName) extra += audioCentralExtra;
    if (name == files.front().first) extra += firstCentralExtra;
    put(row, 30, extra.size(), 2);
    central += row + name + extra + "entry-note"; offset += record.size();
  }
  file.write(central.data(), central.size());
  std::string end64(56, '\0'); put(end64, 0, 0x06064b50, 4); put(end64, 4, 44, 8); put(end64, 12, 45, 2); put(end64, 14, 45, 2);
  put(end64, 24, files.size(), 8); put(end64, 32, files.size(), 8); put(end64, 40, central.size(), 8); put(end64, 48, offset, 8);
  std::string locator(20, '\0'); put(locator, 0, 0x07064b50, 4); put(locator, 8, offset + central.size(), 8); put(locator, 16, 1, 4);
  std::string end(22, '\0'); put(end, 0, 0x06054b50, 4); put(end, 8, files.size(), 2); put(end, 10, files.size(), 2);
  put(end, 12, central.size(), 4); put(end, 16, zip64 ? 0xffffffff : offset, 4); put(end, 20, 12, 2);
  const auto tail = (zip64 ? end64 + locator : std::string{}) + end + "archive-note";
  file.write(tail.data(), tail.size()); require(file.good(), "write ZIP fixture");
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
    std::ofstream(output, std::ios::binary) << std::string(12000, 'f'); return EncodeResult{FileOutcome::Converted, output};
  }, .metrics = &metrics};
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy, MakeStoragePlatformPaths(temp.root / "private", {temp.root}), dependencies);
  require(report.converted == 1 && report.deleted == 1 && !report.recoveryRequired &&
      fs::file_size(path) > prefixBytes && fs::file_size(path) - prefixBytes < 15000 && metrics.sourceBytesRead < 128 * 1024,
      "ZIP64 compaction preserves a huge sparse prefix without reading or staging the hole");
  StorageArchive archive(path, temp.root / "checked.sqlite", {});
  require(archive.comment == "archive-note", "archive comment survives ZIP64 compaction");
  auto first = archive.Next();
  while (first && first->name != "日本語.flac") first = archive.Next(first->offset);
  require(first && first->metadata.ends_with("entry-note"),
      "CP932 stem and entry comment survive renaming");
  archive.Extract(*first, temp.root / "decoded.flac", {});
  std::ifstream input(temp.root / "decoded.flac", std::ios::binary); const std::string bytes((std::istreambuf_iterator<char>(input)), {});
  require(bytes == std::string(12000, 'f'), "rewritten legacy-named payload verifies its CRC");
}
void renamedZipRetainsUnicodePath() {
  using storage_management::archive_detail::number;
  struct NameCase { const char *raw, *unicode, *renamedRaw, *renamedUnicode, *decomposedUnicode; bool centralRequired; };
  for (const NameCase name : {
      NameCase{"\x93\xfa\x96\x7b\x8c\xea.wav", "日本語.wav", "\x93\xfa\x96\x7b\x8c\xea.flac", "日本語.flac", "日本語.flac", false},
      // CP437 names can disagree with CP932 or be invalid in that encoding.
      NameCase{"\xa4.wav", "ñ.wav", "\xa4.flac", "ñ.flac", "n\xcc\x83.flac", true},
      NameCase{"\x82.wav", "é.wav", "\x82.flac", "é.flac", "e\xcc\x81.flac", true}})
  for (bool unicodeCentral : {false, true}) for (bool recover : {false, true}) {
    if (name.centralRequired && !unicodeCentral) continue;
    storage_test::TempDirectory temp; const auto path = temp.root / "unicode.zip";
    writeSparseZip64(path, 0, name.raw, false, name.unicode, unicodeCentral, false);
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    require(review.complete && review.eligibleCount == 1 && review.archives.size() == 1,
        "browsing index recognizes Unicode-named media before Apply");
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    StorageArchiveDependencies deps{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      std::ofstream(output, std::ios::binary) << "converted"; return EncodeResult{FileOutcome::Converted, output};
    }, .faultPoint = [&](std::string_view point) {
      if (recover && point == "archive-entry-ready") throw std::runtime_error("interrupt renamed Unicode media");
    }};
    const auto report = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
    if (recover) {
      require(report.recoveryRequired, "retain prepared Unicode rename for recovery");
      const auto result = RecoverStorageArchives(paths);
      require(!result.failed && !result.recoveryRequired && result.converted == 1, "recover renamed Unicode media");
    } else require(!report.failed && report.converted == 1, "convert legacy media with Unicode path metadata");
    StorageArchive archive(path, temp.root / "checked.sqlite", {});
    auto entry = archive.Next();
    while (entry && entry->name != name.renamedUnicode) entry = archive.Next(entry->offset);
    require(entry && !(entry->flags & 0x800) && entry->rawName == name.renamedRaw,
        "rename retains the original legacy filename encoding");
    const auto validate = [&](std::string_view extra) {
      unsigned matches = 0;
      for (std::size_t at = 0; at < extra.size();) {
        const auto length = number(extra, at + 2, 2);
        require(length <= extra.size() - at - 4, "bounded extra field");
        if (number(extra, at, 2) == 0x7075) {
          ++matches;
          require(number(extra, at + 4, 1) == 1 && number(extra, at + 5, 4) ==
              crc32(0, reinterpret_cast<const Bytef *>(entry->rawName.data()), entry->rawName.size()) &&
              extra.substr(at + 9, length - 5) == name.renamedUnicode, "Unicode path and raw-name CRC match the renamed entry");
        }
        at += 4 + length;
      }
      require(matches == 1, "one updated Unicode path field survives in each ZIP header");
    };
    validate(std::string_view(entry->metadata).substr(46 + entry->rawName.size(), number(entry->metadata, 30, 2)));
    const auto local = archive.io.Read(entry->offset, 30);
    validate(archive.io.Read(entry->offset + 30 + entry->rawName.size(), number(local, 28, 2)));
    const auto files = readArchive(path);
    // libarchive decomposes accented names on macOS. Accept that equivalent
    // reader spelling; the exact Unicode bytes in both headers were checked above.
    const auto readerName = files.contains(name.renamedUnicode) ? name.renamedUnicode : name.decomposedUnicode;
    require(files.contains(readerName) && files.at(readerName) == "converted" && files.at("chart.bms") == "fixture",
        "independent ZIP reader resolves the renamed Unicode path and exact payload");
  }
}
void renamedZipRetainsLocalMetadata() {
  using storage_management::archive_detail::number;
  using storage_management::archive_detail::put;
  // Extended timestamps have more values locally than centrally. UID/GID
  // metadata may exist only locally, so neither can be rebuilt from central data.
  const auto metadata = std::string("\x55\x54\x0d\x00\x07\x00\xf1\x53\x65\x01\xf1\x53\x65\x02\xf1\x53\x65", 17) +
      std::string("\x75\x78\x0b\x00\x01\x04\xe8\x03\x00\x00\x04\xe9\x03\x00\x00", 15);
  const auto centralMetadata = std::string("\x55\x54\x05\x00\x01\x00\xf1\x53\x65", 9) + metadata.substr(17);
  std::string zip64(20, '\0'); put(zip64, 0, 1, 2); put(zip64, 2, 16, 2);
  put(zip64, 4, 10000, 8); put(zip64, 12, 10000, 8);
  for (bool recover : {false, true}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "metadata.zip";
    writeSparseZip64(path, 0, "audio.wav", false, "audio.wav", true, false, metadata + zip64, centralMetadata);
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    require(review.complete && review.eligibleCount == 1 && review.archives.size() == 1, "review local metadata fixture");
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    StorageArchiveDependencies deps{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      std::ofstream(output, std::ios::binary) << "converted"; return EncodeResult{FileOutcome::Converted, output};
    }, .faultPoint = [&](std::string_view point) {
      if (recover && point == "archive-entry-ready") throw std::runtime_error("interrupt local metadata replacement");
    }};
    auto report = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
    if (recover) {
      require(report.recoveryRequired, "retain replacement metadata for recovery");
      report = RecoverStorageArchives(paths);
    }
    require(!report.failed && !report.recoveryRequired && report.converted == 1, "convert media with local ZIP metadata");
    StorageArchive archive(path, temp.root / "checked.sqlite", {});
    auto entry = archive.Next();
    while (entry && entry->name != "audio.flac") entry = archive.Next(entry->offset);
    require(entry.has_value(), "find converted entry with local metadata");
    const auto local = archive.io.Read(entry->offset, 30);
    const auto extra = archive.io.Read(entry->offset + 30 + entry->rawName.size(), number(local, 28, 2));
    std::string retained; unsigned unicodeFields = 0;
    for (std::size_t at = 0; at < extra.size();) {
      const auto tag = number(extra, at, 2), length = number(extra, at + 2, 2);
      require(length <= extra.size() - at - 4, "bounded retained extra field");
      require(tag != 1, "replacement drops obsolete local ZIP64 sizes");
      if (tag == 0x7075) {
        ++unicodeFields;
        require(length == 15 && number(extra, at + 5, 4) ==
            crc32(0, reinterpret_cast<const Bytef *>(entry->rawName.data()), entry->rawName.size()) &&
            extra.substr(at + 9, length - 5) == "audio.flac", "local Unicode name and CRC are rebuilt");
      } else retained += extra.substr(at, length + 4);
      at += length + 4;
    }
    require(retained == metadata && unicodeFields == 1, "local timestamps and Unix ownership survive replacement exactly");
    const auto centralExtra = entry->metadata.substr(46 + entry->rawName.size(), number(entry->metadata, 30, 2));
    require(centralExtra.substr(0, centralMetadata.size()) == centralMetadata &&
        centralExtra.size() == centralMetadata.size() + 19,
        "central timestamps and ownership survive independently of the longer local timestamps");
    require(readArchive(path).at("audio.flac") == "converted", "independent reader extracts media after metadata-preserving replacement");
  }
}
void unsupportedZipReplacementMetadataPreservesEntry() {
  using storage_management::archive_detail::number;
  // A signature depends on the original payload; an unknown vendor field may
  // also encode the filename, CRC, size or alignment. Neither can follow a
  // converted payload, even when it appears only in the central directory.
  for (const auto &[centralOnly, unsupported] : std::vector<std::pair<bool, std::string>>{
      {true, std::string("\x15\x00\x03\x00sig", 7)},
      {true, std::string("\xfe\xca\x03\x00xyz", 7)},
      {false, std::string("\xfe\xca\x03\x00xyz", 7)}}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "unsupported-metadata.zip";
    writeSparseZip64(path, 0, "audio.wav", false, {}, true, false,
        centralOnly ? std::string{} : unsupported, centralOnly ? unsupported : std::string{});
    // Keep the fixed-width fixture names and offsets while adding a selected
    // healthy WAV physically after the unsupported entry.
    {
      std::ifstream input(path, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(input)), {});
      for (auto at = bytes.find("chart.bms"); at != std::string::npos; at = bytes.find("chart.bms", at + 9))
        bytes.replace(at, 9, "later.wav");
      temp.write("unsupported-metadata.zip", bytes);
    }
    std::string original, originalExtra;
    {
      StorageArchive archive(path, temp.root / "original.sqlite", {});
      auto entry = archive.Next();
      while (entry && entry->name != "audio.wav") entry = archive.Next(entry->offset);
      require(entry.has_value(), "find unsupported metadata fixture entry");
      original = archive.io.Read(entry->offset, entry->span);
      originalExtra = entry->metadata.substr(46 + entry->rawName.size(), number(entry->metadata, 30, 2));
    }
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac, .bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    require(review.complete && review.eligibleCount == 3 && review.archives.size() == 1, "review both selected WAV entries and the preceding video");
    StorageArchiveDependencies deps{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      std::ofstream(output, std::ios::binary) << "converted"; return EncodeResult{FileOutcome::Converted, output};
    }};
    const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
        MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
    require(!report.failed && !report.recoveryRequired && report.skipped == 1 && report.converted == 1 && report.deleted == 1,
        "unsupported replacement metadata skips one entry and continues to the healthy WAV");
    require(std::any_of(report.files.begin(), report.files.end(), [](const auto &file) {
      return file.source.filename() == "audio.wav" && file.outcome == FileOutcome::Skipped && file.reason.code == StorageReason::UnsupportedFormat;
    }), "report the unsupported metadata skip");
    StorageArchive archive(path, temp.root / "checked.sqlite", {});
    auto entry = archive.Next();
    while (entry && entry->name != "audio.wav") entry = archive.Next(entry->offset);
    require(entry && archive.io.Read(entry->offset, entry->span) == original, "skipped local record retains its exact metadata and payload");
    require(entry->metadata.substr(46 + entry->rawName.size(), number(entry->metadata, 30, 2)) == originalExtra,
        "skipped entry retains central extras exactly");
    const auto files = readArchive(path);
    require(files.at("audio.wav") == std::string(10000, 'a') && files.at("later.flac") == "converted",
        "independent ZIP reader reads the preserved entry and later conversion");
  }
}
void archiveWideZipSignaturesPreventUpdates() {
  // APPNOTE 4.5.9/4.5.11 put the certificate store and central-directory
  // signature in the first central record. Deleting that entry can orphan
  // other file signatures, and editing any entry invalidates a directory signature.
  for (const auto tag : {0x0014, 0x0016}) for (bool deleteFirst : {false, true}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "signed.zip";
    std::string metadata(4, '\0');
    archive_detail::put(metadata, 0, tag, 2); archive_detail::put(metadata, 2, 3, 2); metadata += "sig";
    writeSparseZip64(path, 0, "audio.wav", false, {}, true, false, {}, {}, metadata);
    std::string error; const auto originalIdentity = ReadFileIdentity(path, error);
    std::ifstream input(path, std::ios::binary); const std::string original((std::istreambuf_iterator<char>(input)), {});
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = deleteFirst ? AudioAction::Keep : AudioAction::LosslessFlac,
            .bga = deleteFirst ? BgaAction::Delete : BgaAction::Keep}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    require(review.complete && review.archives.size() == 1, "review signed ZIP from browsing metadata");
    bool encoded = false;
    StorageArchiveMetrics metrics;
    StorageArchiveDependencies deps{.encode = [&](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      encoded = true; std::ofstream(output, std::ios::binary) << "converted"; return EncodeResult{FileOutcome::Converted, output};
    }, .metrics = &metrics};
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    const auto report = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps);
    require(report.failed == 1 && !report.recoveryRequired && !report.converted && !report.deleted && !encoded && !metrics.bytesWritten,
        "archive-wide signature metadata rejects editing before payload encoding or source writes");
    require(std::any_of(report.files.begin(), report.files.end(), [](const auto &file) {
      return file.reason.code == StorageReason::UnsupportedFormat;
    }), "report unsupported archive-wide signature metadata");
    std::ifstream checked(path, std::ios::binary); const std::string contents((std::istreambuf_iterator<char>(checked)), {});
    require(ReadFileIdentity(path, error) == originalIdentity && contents == original && fs::is_empty(paths.journalRoot),
        "signed archive stays byte-for-byte unchanged without a recovery journal");
  }
}
void unicodeZipFolderSelectionMatchesApply() {
  storage_test::TempDirectory temp; const auto path = temp.root / "unicode-folder.zip";
  writeSparseZip64(path, 0, "\xa4/song.wav", false, "ñ/song.wav", true, false);
  std::filesystem::path selected;
  require(archive_file::visitIndexEntries(path, [&](const auto &entry) {
    if (entry.path.extension() == ".wav") selected = entry.path.parent_path();
    return true;
  }), "index Unicode ZIP folder");
  require(selected == fs::path(u8"ñ"), "browsing uses the checked central Unicode folder");
  InventoryRequest request{.groups = {{"song", path / selected, false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  require(review.complete && review.eligibleCount == 1 && review.archives.size() == 1,
      "review includes the selected Unicode folder's WAV");
  StorageArchiveDependencies deps{.encode = [](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
    std::ofstream(output, std::ios::binary) << "converted"; return EncodeResult{FileOutcome::Converted, output};
  }};
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
  require(!report.failed && report.converted == 1, "Apply converts exactly the reviewed Unicode folder");
  std::vector<unsigned char> payload;
  require(archive_file::readFile(path / fs::path(u8"ñ/song.flac"), payload) &&
      std::string(payload.begin(), payload.end()) == "converted", "renamed Unicode entry remains readable through the indexed reader");
}
void differingLocalUnicodePathsArePreserved() {
  for (bool central : {false, true}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "local-unicode.zip";
    writeSparseZip64(path, 0, "\xa4.wav", false, "ñ.wav", central, false);
    if (central) {
      std::ifstream input(path, std::ios::binary);
      std::string bytes((std::istreambuf_iterator<char>(input)), {}); input.close();
      const auto local = bytes.find("ñ.wav"); require(local != std::string::npos, "find local Unicode path");
      bytes.replace(local, std::string("ñ.wav").size(), "é.wav");
      temp.write("local-unicode.zip", bytes);
    }
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    require(review.eligibleCount == 1 && review.archives.size() == 1, "central metadata permits review without reading local Unicode paths");
    std::string error; const auto before = HashStorageFile(path, {}, error);
    std::atomic<unsigned> encodes = 0;
    StorageArchiveDependencies deps{.encode = [&](const auto &, const auto &, const auto &, auto, auto) {
      ++encodes; return EncodeResult{};
    }};
    const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
        MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
    require(report.failed == 1 && !report.recoveryRequired && encodes == 0 &&
        report.files.front().reason.code == StorageReason::UnsupportedFormat && HashStorageFile(path, {}, error) == before,
        "differing local Unicode names reject Apply before encoding or mutation");
  }
}
void differingBrowsingUnicodePathsArePreserved() {
  storage_test::TempDirectory temp; const auto path = temp.root / "fallback-unicode.zip";
  writeSparseZip64(path, 0, "\xa4/song.wav", false, "ñ/song.wav");
  fs::path selected;
  require(archive_file::visitIndexEntries(path, [&](const auto &entry) {
    if (entry.path.extension() == ".wav") selected = entry.path.parent_path();
    return true;
  }), "index fallback Unicode ZIP folder");
  // Darwin libarchive decomposes the checked Unicode name. Other backends
  // can retain the exact spelling and are covered by the conversion regression.
  if (selected == fs::path(u8"ñ")) return;
  InventoryRequest request{.groups = {{"song", path / selected, false, false, StorageSourceKind::Archive}},
      .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  require(review.eligibleCount == 1 && review.archives.size() == 1, "fallback spelling is eligible during index-only review");
  std::string error; const auto before = HashStorageFile(path, {}, error);
  std::atomic<unsigned> encodes = 0;
  StorageArchiveDependencies deps{.encode = [&](const auto &, const auto &, const auto &, auto, auto) {
    ++encodes; return EncodeResult{};
  }};
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
  require(report.failed == 1 && !report.recoveryRequired && encodes == 0 &&
      report.files.front().reason.code == StorageReason::UnsupportedFormat && HashStorageFile(path, {}, error) == before,
      "different indexed and physical names reject Apply without touching the original");
  const auto indexedPath = (selected / "song.wav").generic_u8string();
  require(report.files.front().reason.detail.find(std::string(indexedPath.begin(), indexedPath.end())) != std::string::npos,
      "browsing mismatch identifies the indexed song whose physical name differs");
}
void unicodeZipPathsAreValidated() {
  for (const auto &unicode : {"../escape.wav", "/absolute.wav", "chart.bms", "\xff.wav"}) {
    storage_test::TempDirectory temp;
    const auto path = temp.root / "unsafe.zip";
    writeSparseZip64(path, 0, "safe.wav", false, unicode);
    bool rejected = false;
    try { StorageArchive archive(path, temp.root / "manifest.sqlite", {}); }
    catch (const StorageArchiveError &) { rejected = true; }
    require(rejected, "authoritative Unicode paths still reject traversal, duplicate names and invalid UTF-8");
  }
  // A stale Unicode path must not override the raw filename after a rename.
  storage_test::TempDirectory temp;
  const auto path = temp.root / "stale.zip";
  writeSparseZip64(path, 0, "safe.wav", false, "other.wav");
  std::ifstream input(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {}); input.close();
  unsigned fields = 0;
  for (std::size_t at = 0; (at = bytes.find("\x75\x70", at)) != std::string::npos; at += 9) {
    bytes.at(at + 5) ^= 1; ++fields; // Invalidate NameCRC32 in both headers.
  }
  require(fields == 2, "fixture contains local and central Unicode fields");
  temp.write("stale.zip", bytes);
  StorageArchive archive(path, temp.root / "manifest.sqlite", {});
  auto entry = archive.Next();
  while (entry && entry->name != "safe.wav") entry = archive.Next(entry->offset);
  require(entry && entry->rawName == "safe.wav", "CRC-mismatched Unicode fields use the original filename");
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
  for (bool archived : {false, true}) {
    const auto root = temp.root / (archived ? "alternatives.zip" : "alternatives");
    if (archived) writeArchive(root, {{"tone.wav", wav}, {"tone.flac", files.at("songs/tone.flac")}});
    else { temp.write("alternatives/tone.wav", wav); temp.write("alternatives/tone.flac", files.at("songs/tone.flac")); }
    OptimizationRequest reuse;
    reuse.policy = reuse.inventory.policy = {.audio = AudioAction::LosslessFlac};
    reuse.inventory.groups = {{"alternatives", root, !archived, false, archived ? StorageSourceKind::Archive : StorageSourceKind::Local}};
    reuse.inventory.scanScratchRoot = temp.root / "scan";
    const auto result = StorageOptimizer(DefaultOptimizerDependencies(MakeStoragePlatformPaths(temp.root / "reuse-private", {temp.root}))).Run(reuse);
    require(result.deleted == 1 && !result.failed && !result.recoveryRequired,
            "real existing FLAC passes full validation and retires a local or archived WAV");
    if (archived) require(readArchive(root).at("tone.flac") == files.at("songs/tone.flac"), "archived target stays byte-identical");
    else require(!fs::exists(root / "tone.wav") && fs::file_size(root / "tone.flac") == files.at("songs/tone.flac").size(),
                 "local existing FLAC is retained without encoding");
  }
}
void archiveQueuedEncodingStopsForSharedRecovery() {
  storage_test::TempDirectory temp; const auto path = temp.root / "queued.zip";
  writeArchive(path, {{"video.mpg", std::string(1000, 'v')}});
  const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::CompressH264}, .scanScratchRoot = paths.scanRoot};
  const auto review = BuildStoragePreflight(request);
  StorageWorkBudget budget({}, [] { return std::optional<std::uint64_t>(100000000); });
  auto first = budget.Acquire(asobmshow::chart_assets::MediaKind::Video, 0, {});
  auto second = budget.Acquire(asobmshow::chart_assets::MediaKind::Video, 0, {});
  unsigned encodes = 0; bool signalled = false;
  StorageArchiveDependencies deps{.encode = [&](const auto &, const auto &, const fs::path &, auto, auto) {
    ++encodes; return EncodeResult{};
  }, .workBudget = &budget};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps,
      [&] { return std::chrono::steady_clock::now() < deadline; }, [&](const StorageProgress &) {
    if (!signalled && fs::exists(paths.journalRoot)) for (const auto &journal : fs::directory_iterator(paths.journalRoot))
      if (fs::exists(journal.path() / "input-1.mpg")) { signalled = true; budget.StopForRecovery(); }
  });
  require(signalled && !encodes && report.failed && !report.cancelled && report.files.size() == 1 &&
      report.files.front().reason.code == StorageReason::RecoveryRequired && fs::is_empty(paths.journalRoot),
      "queued archive work observes shared recovery without depending on its caller checkpoint");
  require(readArchive(path).at("video.mpg") == std::string(1000, 'v'), "shared recovery retains the queued original");
}
void archiveStagingFailurePreservesOriginals() {
  for (const bool afterCommit : {false, true}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "space.zip";
    const std::string original(300000, 'a');
    writeArchive(path, {{"a.wav", original}, {"b.wav", original}});
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = paths.scanRoot};
    const auto review = BuildStoragePreflight(request);
    std::string error; const auto before = HashStorageFile(path, {}, error);
    StorageArchiveDependencies deps{.encode = [](const auto &, const auto &, const fs::path &output, auto, auto) {
      std::ofstream(output) << "tiny"; return EncodeResult{FileOutcome::Converted, output};
    }, .scratchLimit = 8 * 1024 * 1024 + 3 * original.size()};
    bool injected = false;
    const auto report = UpdateStorageArchive(review.archives.front(), request.policy, paths, deps, [&] {
      if (!injected && fs::exists(paths.journalRoot)) for (const auto &journal : fs::directory_iterator(paths.journalRoot)) {
        std::error_code failure;
        const auto bytes = fs::file_size(journal.path() / (afterCommit ? "input-2.wav" : "input-1.wav"), failure);
        if (!failure && bytes > 0) {
          injected = true;
          throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "injected partial input staging failure");
        }
      }
      return true;
    });
    require(injected && report.failed && report.recoveryRequired == afterCommit,
        "partial staging failure reports whether earlier archive changes need recovery");
    if (!afterCommit) require(HashStorageFile(path, {}, error) == before && fs::is_empty(paths.journalRoot),
        "staging failure before mutation leaves archive byte-identical and removes partial input");
    else {
      require(!fs::is_empty(paths.journalRoot), "staging failure after a commit retains its recovery journal");
      const auto recovered = RecoverStorageArchives(paths);
      require(!recovered.failed && !recovered.recoveryRequired && fs::is_empty(paths.journalRoot),
          "recovery after space returns removes partial input and finishes the archive");
    }
    const auto files = readArchive(path);
    require(files.size() == 2 && files.at("b.wav") == original &&
        (afterCommit ? files.at("a.flac") == "tiny" : files.at("a.wav") == original),
        "staging failure preserves the failed entry and any earlier accepted conversion");
  }
}
void archivePolicyBoundaries() {
  for (const std::string mode : {"compressed", "budget", "cancel", "failed", "throw"}) {
    storage_test::TempDirectory temp; const auto path = temp.root / "songs.zip";
    writeArchive(path, {{"a.wav", std::string(10000, 'a')}, {"b.wav", std::string(9000, 'b')}, {"chart.bms", "chart"}}, mode == "compressed");
    InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
        .policy = {.audio = AudioAction::LosslessFlac}, .scanScratchRoot = temp.root / "scan"};
    const auto review = BuildStoragePreflight(request);
    const auto paths = MakeStoragePlatformPaths(temp.root / "private", {temp.root});
    std::string error; const auto before = ReadFileIdentity(path, error); std::atomic<bool> cancel = false; std::atomic<unsigned> calls = 0;
    StorageArchiveDependencies dependencies{.encode = [&](const AssetCandidate &asset, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      ++calls;
      std::ifstream source(asset.source); const bool second = source.get() == 'b'; source.close();
      if (second && mode == "throw") throw std::runtime_error("injected encoder exception");
      if (second && mode == "failed") return EncodeResult{.outcome = FileOutcome::Failed, .reason = {StorageReason::ValidationFailed, "injected validation failure"}};
      std::ofstream(output, std::ios::binary) << std::string(1000, 'f'); return EncodeResult{FileOutcome::Converted, output};
    }, .availableSpace = [&](const fs::path &) -> std::optional<std::uint64_t> { return mode == "budget" ? 1024 : 100000000; }};
    const auto result = UpdateStorageArchive(review.archives.front(), request.policy, paths, dependencies, [&] { return !cancel; },
        [&](const StorageProgress &progress) { if (mode == "cancel" && progress.phase == StoragePhase::Commit) cancel = true; });
    require(!result.recoveryRequired && fs::is_empty(paths.journalRoot), "ordinary skips, cancellation and encoder failures settle the archive");
    const auto files = readArchive(path); require(files.at("chart.bms") == "chart", "policy boundary preserves charts");
    if (mode == "compressed") {
      require(result.converted == 2 && !result.skipped && files.at("a.flac") == std::string(1000, 'f') &&
          files.at("b.flac") == std::string(1000, 'f'), "lossless FLAC replaces even better-compressed WAV records");
    } else if (mode == "budget") {
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
      std::atomic<unsigned> encodes = 0;
      StorageArchiveDependencies deps{.encode = [&](const auto &, const auto &, const auto &, auto, auto) {
        ++encodes; return EncodeResult{};
      }};
      const auto result = UpdateStorageArchive(review.archives.front(), request.policy,
          MakeStoragePlatformPaths(temp.root / "private", {temp.root}), deps);
      require(result.failed == 1 && !result.recoveryRequired && encodes == 0 && HashStorageFile(path, {}, error) == before,
          "indexed candidates with unsafe layouts are rejected before encoding or mutation");
    } else if (mode == "collision") {
      require(review.eligibleCount == 1, "existing archive destination is eligible for validation");
      auto deps = DefaultOptimizerDependencies(MakeStoragePlatformPaths(temp.root / "private", {temp.root}));
      OptimizationRequest input{.inventory = request, .policy = request.policy, .preflight = review};
      const auto result = StorageOptimizer(deps).Run(input);
      require(result.failed == 1 && !result.deleted && readArchive(path).size() == 2,
              "an invalid existing FLAC preserves both archive alternatives");
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
    if (mode == "symlink") std::ofstream(privateRoot / "foreign") << std::string(1000, 'f');
    StorageArchiveDependencies deps{.encode = [&](const AssetCandidate &, const StoragePolicy &, const fs::path &output, Checkpoint, ProgressCallback) {
      if (mode == "symlink") {
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
void storageArchiveCompactionBenchmark(std::size_t count, std::size_t payloadBytes, bool deleteFirst) {
  storage_test::TempDirectory temp;
  const auto path = temp.root / "compact.zip";
  const std::string payload(payloadBytes, 'i');
  std::vector<std::pair<std::string, std::string>> files;
  if (deleteFirst) files.emplace_back("movie.avi", "video");
  for (std::size_t i = 0; i < count; ++i) files.emplace_back(std::to_string(i) + ".png", payload);
  if (!deleteFirst) files.emplace_back("movie.avi", "video");
  writeArchive(path, files);
  InventoryRequest request{.groups = {{"all", path, false, false, StorageSourceKind::Archive}},
      .policy = {.bga = BgaAction::Delete}, .scanScratchRoot = temp.root / "scan"};
  const auto review = BuildStoragePreflight(request);
  StorageArchiveMetrics metrics;
  const auto report = UpdateStorageArchive(review.archives.front(), request.policy,
      MakeStoragePlatformPaths(temp.root / "private", {temp.root}), {.metrics = &metrics});
  require(report.deleted == 1 && !report.failed && !report.recoveryRequired, "benchmark archive compacts successfully");
  const auto actual = readArchive(path);
  require(actual.size() == count, "benchmark retains all non-deleted entries");
  for (std::size_t i = 0; i < count; ++i) require(actual.at(std::to_string(i) + ".png") == payload, "benchmark retains exact payloads");
  std::cout << "archive compaction entries=" << count << " payload=" << payloadBytes << " delete_first=" << deleteFirst
      << " batches=" << metrics.durableBatches << " write_ranges=" << metrics.sourceWriteRanges << " read_bytes=" << metrics.sourceBytesRead << " write_bytes=" << metrics.bytesWritten
      << " parallel_hash_batches=" << metrics.parallelHashBatches << " hash_workers=" << metrics.peakHashWorkers
      << " total_ms=" << metrics.totalNanoseconds / 1e6 << " prepare_ms=" << metrics.preparationNanoseconds / 1e6
      << " read_ms=" << metrics.readNanoseconds / 1e6 << " hash_ms=" << metrics.hashNanoseconds / 1e6
      << " redo_ms=" << metrics.redoNanoseconds / 1e6 << " journal_ms=" << metrics.journalNanoseconds / 1e6
      << " write_ms=" << metrics.writeNanoseconds / 1e6 << " sync_ms=" << metrics.syncNanoseconds / 1e6 << '\n';
}
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
void storageArchiveParallelTests() { archiveQueuedEncodingStopsForSharedRecovery(); archiveStagingFailurePreservesOriginals(); archiveReadAheadKeepsFullAudioCapacityWithVideos(); archiveFailureStopsPeersBeforeJoiningEncoders(); archiveEncodersOverlapAndDrainSafely(); independentArchivesAndDirectoriesShareEncodingCapacity(); archiveLookaheadRespectsAggregateScratch(); }
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
      {"parallel-encoders", archiveEncodersOverlapAndDrainSafely},
      {"parallel-owners", independentArchivesAndDirectoriesShareEncodingCapacity},
      {"parallel-media-read-ahead", archiveReadAheadKeepsFullAudioCapacityWithVideos},
      {"parallel-scratch", archiveLookaheadRespectsAggregateScratch},
      {"staging-disk-full", archiveStagingFailurePreservesOriginals},
      {"queued-recovery", archiveQueuedEncodingStopsForSharedRecovery},
      {"parallel-failure", archiveFailureStopsPeersBeforeJoiningEncoders},
      {"container-relocation", archiveRecoveryAfterContainerRelocation},
      {"measurement-deltas", archiveChangesUpdateCachedInnerMeasurements},
      {"sparse-prefix-recovery", sparsePrefixRecovery}, {"large-batch-recovery", archiveLargeRecordRecovery}, {"delete-batches", archiveConsecutiveDeletesAreBatched},
      {"manifest", manifestsExtractEntries}, {"preflight", selectedArchiveFoldersAreEligible},
      {"case-sensitive-selection", archiveFolderSelectionPreservesCase},
      {"deflate-boundary", deflatedPayloadRefillsAtExactOutputBoundary},
      {"sandbox", archiveReviewSeparatesPrivateAndLibraryAccess}, {"sandbox-staging", archiveStagingRespectsPrivateBoundary},
      {"cached-review", reviewUsesCachedIndexAndApplyValidatesLayout},
      {"system-entries", reviewAndApplyPreserveSystemEntries},
      {"explicit-directories", explicitArchiveDirectoriesSurviveRepeatedUpdates},
      {"damaged-payload", damagedPayloadDoesNotAbortOtherSelectedSongs},
      {"metadata-preparation", archivePreparationDoesNotReadPayloads},
      {"metadata-traversal", archiveTraversalUsesOrderedMetadataReads},
      {"descriptor-boundaries", archiveDescriptorBoundaries},
      {"physical-savings", archiveSavingsUsePhysicalSize},
      {"batched-compaction", archiveCompactionBatchesSmallRecords}, {"prefix-checkpoints", unchangedArchivePrefixAvoidsFrequentCheckpoints},
      {"small-record-recovery", smallArchiveRecordsRecoverAcrossBatches},
      {"prepared-hash-failure", preparedArchiveHashFailurePrecedesWrites},
      {"growth-batches", growingArchiveMultiBatchRecovery}, {"growth", growingFlacArchiveRecovery}, {"update", archiveUpdatesBatchWithoutFullCopy}, {"recovery", interruptedArchiveWritesRecover},
      {"unicode-rename", renamedZipRetainsUnicodePath},
      {"local-metadata", renamedZipRetainsLocalMetadata},
      {"unsupported-replacement-metadata", unsupportedZipReplacementMetadataPreservesEntry},
      {"archive-wide-signatures", archiveWideZipSignaturesPreventUpdates},
      {"unicode-folder", unicodeZipFolderSelectionMatchesApply},
      {"unicode-local-disagreement", differingLocalUnicodePathsArePreserved},
      {"unicode-index-disagreement", differingBrowsingUnicodePathsArePreserved},
      {"unicode-validation", unicodeZipPathsAreValidated},
      {"zip64", sparseZip64AndLegacyNames}, {"invalid-encoding", malformedLegacyNamesAreRejected},
      {"playback", realArchiveOptimizerPreservesPlayback}, {"existing-destinations", existingArchiveDestinationsAreReused},
      {"case-distinct-destinations", archiveReuseRequiresExactParentAndStem},
      {"boundaries", archivePolicyBoundaries}, {"rejections", unsafeAndStaleArchivesAreRejected},
      {"partial", partialWritesAndForeignChanges}}) {
    try { run(); } catch (const std::exception &error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
  }
}
