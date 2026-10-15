#include "ArchiveFile.h"
#include "RAII.h"
#include "Uuid.h"
#include "audio/DecodedAssetCache.h"
#include "audio/SoundFileIO.h"
#include "audio/decoder.h"

#include <archive.h>
#include <archive_entry.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void writeWav(const std::filesystem::path &path, int channels = 2, int frames = 12017) {
  SF_INFO info{};
  info.samplerate = 32000;
  info.channels = channels;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  auto file = asobmashow::audio::openSoundFileHandle(path, SFM_WRITE, info);
  require(bool(file), "create WAV");
  std::vector<short> samples(static_cast<std::size_t>(frames) * channels);
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<short>((i * 31) % 60001 - 30000);
  require(sf_writef_short(file.get(), samples.data(), frames) == frames, "write WAV");
}
std::vector<unsigned char> bytesAt(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void zipWav(const std::filesystem::path &zip, const std::vector<unsigned char> &bytes) {
  UniqueResource<struct archive, archive_write_free> writer(archive_write_new());
  require(bool(writer), "create ZIP writer");
  require(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK, "set ZIP format");
  require(archive_write_open_filename(writer.get(), zip.string().c_str()) == ARCHIVE_OK, "open ZIP");
  UniqueResource<struct archive_entry, archive_entry_free> entry(archive_entry_new());
  archive_entry_set_pathname(entry.get(), "sounds/tone.wav");
  archive_entry_set_size(entry.get(), bytes.size());
  archive_entry_set_filetype(entry.get(), AE_IFREG);
  archive_entry_set_perm(entry.get(), 0644);
  require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK, "write ZIP header");
  require(archive_write_data(writer.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()), "write ZIP bytes");
  require(archive_write_close(writer.get()) == ARCHIVE_OK, "close ZIP");
}
void testSparseReuse(const std::filesystem::path &root) {
  const auto path = root / "tone.wav";
  writeWav(path);
  std::atomic_bool cancelled{false};
  SF_INFO info{};
  std::vector<short> expected;
  require(decodeAudioToPCM(fspath_to_path_t(path), expected, info, cancelled), "legacy decode");
  audio::DecodedAssetCache cache(root);
  std::vector<short> range;
  require(cache.ReadRange(path, 7001, 501, range, info, cancelled), "read nonzero preview range");
  require(range == std::vector<short>(expected.begin() + 7001 * 2, expected.begin() + 7502 * 2),
          "nonzero range must preserve native channels and conversion");
  require(cache.GetStatistics().decodedFrames == 4096, "preview must decode just its intersecting page");
  std::vector<short> full;
  require(cache.Materialize(path, full, info, cancelled), "materialize gameplay PCM");
  require(full == expected, "materialized PCM must equal legacy decoder exactly");
  require(cache.GetStatistics().decodedFrames == 12017, "gameplay must decode only missing pages");
  require(cache.GetStatistics().cacheHitFrames >= 4096, "gameplay must consume prepared page");
  require(cache.ReadRange(path, 12000, 1000, range, info, cancelled) && range.size() == 34,
          "tail range must clip at EOF");
}
void testCancellationRetainsReadyPages(const std::filesystem::path &root) {
  const auto path = root / "cancel.wav";
  writeWav(path);
  audio::DecodedAssetCache cache(root);
  std::atomic_bool cancelled{false};
  SF_INFO info{};
  std::vector<short> pcm;
  require(cache.ReadRange(path, 0, 17, pcm, info, cancelled), "prepare cache page");
  cancelled = true;
  require(!cache.Materialize(path, pcm, info, cancelled) && pcm.empty(), "cancelled gameplay load must not expose partial PCM");
  require(cache.Contains(path), "request cancellation must not erase prepared pages");
  cancelled = false;
  std::stop_source stop;
  stop.request_stop();
  require(!cache.ReadRange(path, 5000, 20, pcm, info, cancelled, stop.get_token()) && pcm.empty(),
          "stop token must stop even cached metadata requests");
  require(cache.Materialize(path, pcm, info, cancelled), "next session request can complete");
  require(cache.GetStatistics().decodedFrames == 12017, "cancelled preview's page must still be reused");
}
void testPreloadRetainsAllPagesForGameplay(const std::filesystem::path &root) {
  const auto path = root / "preload.wav";
  writeWav(path);
  audio::DecodedAssetCache cache(root, audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  std::atomic_bool cancelled{false}; SF_INFO info{}; std::vector<short> pcm;
  require(cache.ReadRange(path, 7001, 17, pcm, info, cancelled), "prepare highlight page");
  require(cache.Preload(path, cancelled), "preload the remaining pages");
  require(cache.GetStatistics().decodedFrames == 12017 &&
              cache.GetStatistics().retainedBytes == 12017 * 2 * sizeof(short),
          "preloading must retain the entire asset without decoding ready pages again");
  cancelled = true;
  require(!cache.Preload(path, cancelled), "preload must honor cancellation even for warm assets");
  cancelled = false;
  require(cache.Materialize(path, pcm, info, cancelled) && pcm.size() == 12017 * 2,
          "gameplay must materialize all preloaded audio");
  require(cache.GetStatistics().decodedFrames == 12017,
          "materializing preloaded audio must not invoke the decoder again");
}
void testCancellationDuringFullDecode(const std::filesystem::path &root) {
  const auto path = root / "cancel-active.wav";
  writeWav(path, 2, 8 * 1024 * 1024);
  audio::DecodedAssetCache cache(root, 16384);
  std::atomic_bool cancelled{false}; SF_INFO info{}; std::vector<short> pcm;
  require(cache.ReadRange(path, 7001, 17, pcm, info, cancelled), "warm page before active cancellation");
  auto pending = std::async(std::launch::async, [&] {
    return cache.Materialize(path, pcm, info, cancelled);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cache.GetStatistics().decodedFrames <= 4096 &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  const bool observedProgress = cache.GetStatistics().decodedFrames > 4096;
  cancelled = true;
  require(observedProgress, "full decode did not make progress");
  require(!pending.get() && pcm.empty(), "active cancellation must discard partial gameplay output");
  const auto decoded = cache.GetStatistics().decodedFrames;
  cancelled = false;
  require(cache.ReadRange(path, 7001, 17, pcm, info, cancelled), "reuse page after cancelling full load");
  require(cache.GetStatistics().decodedFrames == decoded, "active cancellation must preserve preview page");
}
void testEvictionAndSourceChange(const std::filesystem::path &root) {
  const auto path = root / "bounded.wav";
  writeWav(path);
  audio::DecodedAssetCache cache(root, 4096 * 2 * sizeof(short));
  std::atomic_bool cancelled{false}; SF_INFO info{}; std::vector<short> pcm;
  require(cache.ReadRange(path, 0, 1, pcm, info, cancelled), "first bounded page");
  require(cache.ReadRange(path, 5000, 1, pcm, info, cancelled), "second bounded page");
  require(cache.GetStatistics().retainedBytes <= 16384, "retained PCM budget must hold");
  require(cache.ReadRange(path, 0, 1, pcm, info, cancelled), "evicted page can be decoded again");
  require(cache.GetStatistics().decodedFrames == 12288, "old page was evicted instead of exceeding budget");
  writeWav(path, 1, 53);
  require(!cache.Contains(path), "changed file must invalidate its previous identity");
  require(cache.Materialize(path, pcm, info, cancelled) && info.channels == 1 && pcm.size() == 53,
          "changed metadata must replace old pages");
}
void testFullLoadPreservesLatePreviewPages(const std::filesystem::path &root) {
  const auto path = root / "highlight.wav";
  writeWav(path);
  audio::DecodedAssetCache cache(root, 16384);
  std::atomic_bool cancelled{false}; SF_INFO info{}; std::vector<short> pcm;
  require(cache.ReadRange(path, 9000, 17, pcm, info, cancelled), "prepare late highlight page");
  require(cache.Materialize(path, pcm, info, cancelled), "load whole asset around retained highlight");
  require(pcm.size() == 12017 * 2, "gameplay must receive complete native PCM");
  require(cache.GetStatistics().decodedFrames == 12017,
          "full load must not evict and decode its late preview page again");
  require(cache.GetStatistics().retainedBytes <= 16384, "full load must respect retained budget");
}
void testInspectDistinguishesInvalidAndOperationalFailures(const std::filesystem::path &root) {
  std::atomic_bool cancelled{false}; SF_INFO info{};
  audio::DecodedAssetCache::FailureKind failure;
  audio::DecodedAssetCache cache(root);
  require(!cache.Inspect(root / "missing.wav", info, cancelled, {}, nullptr, &failure) &&
              failure == audio::DecodedAssetCache::FailureKind::InvalidAsset,
          "missing keysound must be classified separately from preparation failure");
  cancelled = true;
  require(!cache.Inspect(root / "missing.wav", info, cancelled, {}, nullptr, &failure) &&
              failure == audio::DecodedAssetCache::FailureKind::Cancelled,
          "cancellation must have its own failure classification");
  cancelled = false;
  const auto wav = root / "failure-tone.wav";
  writeWav(wav);
  const auto zip = root / "failure.zip";
  zipWav(zip, bytesAt(wav));
  const auto notDirectory = root / "cache-is-file";
  { std::ofstream file(notDirectory); file << "not a directory"; }
  audio::DecodedAssetCache brokenCache(notDirectory);
  require(!brokenCache.Inspect(archive_file::makeVirtualPath(zip, "sounds/tone.wav"),
                              info, cancelled, {}, nullptr, &failure) &&
              failure == audio::DecodedAssetCache::FailureKind::Operation,
          "cache staging failure must not be treated as a missing/invalid keysound");
}
void testArchiveReuse(const std::filesystem::path &root) {
  const auto path = root / "archive-tone.wav";
  writeWav(path);
  const auto bytes = bytesAt(path);
  const auto zip = root / "sounds.zip";
  zipWav(zip, bytes);
  const auto logical = archive_file::makeVirtualPath(zip, "sounds/tone.wav");
  std::atomic_bool cancelled{false}; SF_INFO info{}; std::vector<short> expected;
  require(decodeAudioBytesToPCM(fspath_to_path_t(logical), bytes, expected, info, cancelled), "legacy archive PCM");
  audio::DecodedAssetCache cache(root);
  std::vector<short> pcm;
  require(cache.ReadRange(logical, 9000, 19, pcm, info, cancelled), "archive preview page");
  require(cache.Contains(logical), "cache lookup uses original virtual asset identity");
  require(cache.Materialize(logical, pcm, info, cancelled) && pcm == expected, "archive gameplay reuses extracted source");
  require(cache.GetStatistics().decodedFrames == 12017, "archive cached page must not be decoded again");
  audio::DecodedAssetCache batchCache(root);
  require(batchCache.MaterializeBytes(logical, bytes, pcm, info, cancelled) && pcm == expected,
          "batch archive bytes must seed native PCM pages");
  const auto before = batchCache.GetStatistics().decodedFrames;
  require(batchCache.ReadRange(logical, 9000, 19, pcm, info, cancelled), "read batch-seeded page");
  require(batchCache.GetStatistics().decodedFrames == before, "batch-seeded PCM must be reusable");
}
}
int main() {
  const auto root = std::filesystem::temp_directory_path() / ("decoded-asset-tests-" + uuid::generateV4());
  try {
    std::filesystem::create_directories(root);
    testSparseReuse(root);
    testPreloadRetainsAllPagesForGameplay(root);
    testCancellationRetainsReadyPages(root);
    testCancellationDuringFullDecode(root);
    testEvictionAndSourceChange(root);
    testFullLoadPreservesLatePreviewPages(root);
    testInspectDistinguishesInvalidAndOperationalFailures(root);
    testArchiveReuse(root);
    std::filesystem::remove_all(root);
    std::cout << "decoded asset cache tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::filesystem::remove_all(root);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
