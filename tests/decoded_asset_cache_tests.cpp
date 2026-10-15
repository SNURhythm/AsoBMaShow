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
void testProgressiveReadsReuseDecoderAcrossMetadataAndSeeks(const std::filesystem::path &root) {
  const auto path = root / "progressive.ogg";
  SF_INFO written{};
  written.samplerate = 32000;
  written.channels = 1;
  written.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
  {
    auto file = asobmashow::audio::openSoundFileHandle(path, SFM_WRITE, written);
    require(bool(file), "create compressed progressive fixture");
    std::vector<short> samples(96000);
    for (std::size_t index = 0; index < samples.size(); ++index)
      samples[index] = static_cast<short>(int(index % 2000) * 20 - 20000);
    require(sf_writef_short(file.get(), samples.data(), samples.size()) ==
                static_cast<sf_count_t>(samples.size()), "write progressive fixture");
  }
  std::atomic_bool cancelled{false}; SF_INFO info{}; std::vector<short> expected;
  require(decodeAudioToPCM(fspath_to_path_t(path), expected, info, cancelled),
          "decode progressive reference");
  audio::DecodedAssetCache cache(root, audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  require(cache.Inspect(path, info, cancelled), "inspect progressive source");
  // Start at a highlight, stream forward in small requests, then warm its prefix.
  std::vector<short> pcm;
  for (int first : {32000, 40000, 48000, 56000, 64000, 72000, 80000, 88000,
                    0, 8000, 16000, 24000}) {
    require(cache.ReadRange(path, first, 8000, pcm, info, cancelled),
            "read progressive compressed range");
    require(std::equal(pcm.begin(), pcm.end(), expected.begin() + first),
            "reused decoder must preserve the cursor through forward/backward reads");
  }
  require(cache.GetStatistics().decodedFrames == 96000,
          "progressive reads must retain all native frames");
  require(cache.GetStatistics().decoderOpens == 1,
          "metadata and successive preview windows must reuse one compressed decoder");
}

void testDecoderRetentionRemainsBounded(const std::filesystem::path &root) {
  audio::DecodedAssetCache cache(root);
  std::atomic_bool cancelled{false}; SF_INFO info{};
  std::vector<std::filesystem::path> paths;
  for (int index = 0; index < 40; ++index) {
    paths.push_back(root / ("decoder-" + std::to_string(index) + ".wav"));
    writeWav(paths.back(), 1);
    require(cache.Inspect(paths.back(), info, cancelled), "inspect bounded decoder fixture");
  }
  require(cache.GetStatistics().liveDecoders <= 16,
          "idle decoder retention must not grow with the chart's asset count");
  const auto opened = cache.GetStatistics().decoderOpens;
  std::vector<short> pcm;
  require(cache.ReadRange(paths.back(), 5000, 100, pcm, info, cancelled),
          "read most recent decoder fixture");
  require(cache.GetStatistics().decoderOpens == opened,
          "the most recently used idle decoder must remain reusable");
  std::vector<std::future<bool>> reads;
  for (const auto &path : paths) {
    reads.push_back(std::async(std::launch::async, [&, path] {
      SF_INFO readInfo{}; std::vector<short> range;
      return cache.ReadRange(path, 8000, 4000, range, readInfo, cancelled);
    }));
  }
  for (auto &read : reads) require(read.get(), "concurrent decoder borrower failed");
  require(cache.GetStatistics().peakLiveDecoders <= 16,
          "active and retained idle decoders must share the descriptor limit");
}

void testScopedDecoderPinsAndUnrelatedBorrowers(const std::filesystem::path &root) {
  audio::DecodedAssetCache cache(root, audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  std::atomic_bool cancelled{false}; SF_INFO info{};
  std::vector<std::filesystem::path> paths;
  std::vector<audio::DecodedAssetCache::DecoderPin> pins;
  for (int index = 0; index < 24; ++index) {
    paths.push_back(root / ("pinned-" + std::to_string(index) + ".wav"));
    writeWav(paths.back(), 1);
    pins.push_back(cache.PinDecoder(paths.back(), cancelled));
    require(bool(pins.back()), "pin progressive decoder");
    require(cache.GetStatistics().decoderOpens == static_cast<std::uint64_t>(index),
            "pinning must not open or decode audio");
    require(cache.Inspect(paths.back(), info, cancelled), "inspect pinned source");
  }
  auto duplicate = cache.PinDecoder(paths.front(), cancelled);
  require(bool(duplicate) && cache.GetStatistics().pinnedEntries == 24,
          "multiple pins of one entry must share one reservation");
  std::vector<std::future<bool>> unrelated;
  for (int index = 0; index < 40; ++index) {
    const auto path = root / ("unpinned-" + std::to_string(index) + ".wav");
    writeWav(path, 1);
    unrelated.push_back(std::async(std::launch::async, [&, path] {
      SF_INFO metadata{}; std::vector<short> pcm;
      return cache.ReadRange(path, 0, 17, pcm, metadata, cancelled);
    }));
  }
  for (auto &read : unrelated) require(read.get(), "unrelated borrower must progress around pins");
  require(cache.GetStatistics().peakLiveDecoders <= 24 + 16,
          "unrelated borrowers must still share the unpinned descriptor bound");
  const auto opened = cache.GetStatistics().decoderOpens;
  for (const auto &path : paths) {
    std::vector<short> pcm;
    require(cache.ReadRange(path, 8000, 100, pcm, info, cancelled), "reuse pinned cursor");
  }
  require(cache.GetStatistics().decoderOpens == opened,
          "unrelated borrowers must never evict explicitly pinned cursors");
  pins.clear();
  require(cache.GetStatistics().pinnedEntries == 1 && cache.GetStatistics().liveDecoders <= 17,
          "last-pin accounting must preserve duplicate ownership and trim other idle cursors");
  duplicate = {};
  require(cache.GetStatistics().pinnedEntries == 0 && cache.GetStatistics().liveDecoders <= 16,
          "last unpin must immediately restore bounded idle retention");
  const auto decoded = cache.GetStatistics().decodedFrames;
  std::vector<short> pcm;
  require(cache.ReadRange(paths.front(), 8000, 100, pcm, info, cancelled) &&
              cache.GetStatistics().decodedFrames == decoded,
          "releasing decoder pins must preserve retained PCM");
}

void testDecoderPinIdentityAndOwnerLifetime(const std::filesystem::path &root) {
  const auto path = root / "pin-identity.wav";
  writeWav(path, 1);
  std::atomic_bool cancelled{false}; SF_INFO info{};
  audio::DecodedAssetCache cache(root);
  auto oldPin = cache.PinDecoder(path, cancelled);
  require(bool(oldPin) && cache.Inspect(path, info, cancelled), "inspect pinned old identity");
  writeWav(path, 1, 53);
  std::vector<short> pcm;
  require(cache.Materialize(path, pcm, info, cancelled) && pcm.size() == 53,
          "pinned old identity must not hide a replacement source");
  require(cache.GetStatistics().liveDecoders == 1 && cache.GetStatistics().decoderOpens == 2,
          "invalidation must close an old pinned idle decoder");
  oldPin = {};
  require(cache.GetStatistics().pinnedEntries == 0 && cache.Contains(path),
          "old pin release must not affect the replacement's retained pages");
  cancelled = true;
  require(!cache.PinDecoder(path, cancelled) && cache.GetStatistics().pinnedEntries == 0,
          "cancelled pin acquisition must leave no reservation");
  cancelled = false;
  const auto archive = root / "pin-lifetime.zip";
  zipWav(archive, bytesAt(path));
  const auto scratch = root / "pin-lifetime-cache";
  audio::DecodedAssetCache::DecoderPin surviving;
  {
    auto owner = std::make_unique<audio::DecodedAssetCache>(scratch);
    surviving = owner->PinDecoder(archive / "sounds/tone.wav", cancelled);
    require(bool(surviving) && owner->Inspect(archive / "sounds/tone.wav", info, cancelled),
            "stage pinned archive source");
    owner.reset();
    require(!std::filesystem::is_empty(scratch), "live pin must keep its cache state alive");
  }
  surviving = {};
  require(std::filesystem::is_empty(scratch),
          "a pin outliving the cache must safely close descriptors and remove staging on release");
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
void testArchiveStagingRetainsBytesWithoutPcmOrRepeatedExtraction(const std::filesystem::path &root) {
  const auto path = root / "staged-tone.wav";
  writeWav(path);
  const auto zip = root / "staged.zip";
  zipWav(zip, bytesAt(path));
  const std::vector<std::filesystem::path> paths{
      archive_file::makeVirtualPath(zip, "sounds/tone.wav")};
  const auto scratch = root / "staged-cache";
  std::atomic_bool cancelled{false};
  {
    audio::DecodedAssetCache cache(scratch);
    require(cache.StageArchiveAssets(paths, cancelled), "stage archived source");
    auto state = cache.GetStatistics();
    require(state.archiveReads == 1 && state.decodedFrames == 0 && state.decoderOpens == 0,
            "staging must extract encoded bytes without decoding full PCM");
    cancelled = true;
    require(!cache.StageArchiveAssets(paths, cancelled),
            "staging must observe cancellation even for already staged paths");
    cancelled = false;
    auto restage = std::async(std::launch::async, [&] {
      return cache.StageArchiveAssets(paths, cancelled);
    });
    auto materialize = std::async(std::launch::async, [&] {
      SF_INFO info{}; std::vector<short> pcm;
      return cache.Materialize(paths.front(), pcm, info, cancelled) && pcm.size() == 24034;
    });
    require(restage.get() && materialize.get(),
            "staging and gameplay decode must serialize without losing the staged source");
    require(cache.GetStatistics().archiveReads == 1,
            "concurrent reuse must not extract an already staged source again");
  }
  require(std::filesystem::is_empty(scratch),
          "cache destruction must close retained decoders and remove staged files");
}
}
int main() {
  const auto root = std::filesystem::temp_directory_path() / ("decoded-asset-tests-" + uuid::generateV4());
  try {
    std::filesystem::create_directories(root);
    testSparseReuse(root);
    testProgressiveReadsReuseDecoderAcrossMetadataAndSeeks(root);
    testDecoderRetentionRemainsBounded(root);
    testScopedDecoderPinsAndUnrelatedBorrowers(root);
    testDecoderPinIdentityAndOwnerLifetime(root);
    testPreloadRetainsAllPagesForGameplay(root);
    testCancellationRetainsReadyPages(root);
    testCancellationDuringFullDecode(root);
    testEvictionAndSourceChange(root);
    testFullLoadPreservesLatePreviewPages(root);
    testInspectDistinguishesInvalidAndOperationalFailures(root);
    testArchiveReuse(root);
    testArchiveStagingRetainsBytesWithoutPcmOrRepeatedExtraction(root);
    std::filesystem::remove_all(root);
    std::cout << "decoded asset cache tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::filesystem::remove_all(root);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
