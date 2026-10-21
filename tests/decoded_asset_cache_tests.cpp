#include "ArchiveFile.h"
#include "RAII.h"
#include "Uuid.h"
#include "audio/DecodedAssetCache.h"
#include "audio/ChartAudioFileLookup.h"
#include "audio/SoundFileIO.h"
#include "audio/decoder.h"
#include "support/AllocationFailure.h"

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
void testChartAudioFileLookup(const std::filesystem::path &root) {
  const auto folder = root / "lookup";
  std::filesystem::create_directory(folder);
  std::ofstream(folder / "tone.ogg") << "ogg";
  std::ofstream(folder / "priority.wav") << "wav";
  std::ofstream(folder / "priority.ogg") << "ogg";
  std::atomic_bool cancelled{false};
  audio::ChartAudioFileLookup lookup(cancelled);
  const std::vector<std::string_view> extensions{".wav", ".ogg"};
  const auto check = [&](const std::filesystem::path &path) {
    require(lookup.Find(path, extensions) == archive_file::findFileWithExtensions(path, extensions),
            "directory lookup preserves extension resolution");
  };
  check(folder / "tone.wav");
  check(folder / "priority.ogg");
  check(folder / "absent.wav");
  check(folder / "TONE.wav");
  // A directory change must invalidate negative results in this same batch.
  const auto oldTime = std::filesystem::last_write_time(folder);
  std::ofstream(folder / "new.wav") << "new";
  std::filesystem::last_write_time(folder, oldTime + std::chrono::seconds(1));
  check(folder / "new.wav");
  check(folder / "absent.wav"); // Rebuild the negative snapshot after refresh.
  const auto restoredTime = std::filesystem::last_write_time(folder);
  std::ofstream(folder / "restored.wav") << "new with restored directory mtime";
  std::filesystem::last_write_time(folder, restoredTime);
  check(folder / "restored.wav");
#ifndef _WIN32
  std::filesystem::create_symlink(folder / "missing-target", folder / "dangling.wav");
  std::ofstream(folder / "dangling.ogg") << "ogg";
  check(folder / "dangling.wav");
#endif
  std::ofstream(folder / std::filesystem::path(u8"音.ogg")) << "ogg";
  check(folder / std::filesystem::path(u8"音.wav"));
  check(folder / "tone.wav");
  cancelled = true;
  require(!lookup.Find(folder / "tone.wav", extensions), "lookup observes cancellation");
}
void testWholeLoadAndConcurrentReuse(const std::filesystem::path &root) {
  const auto path = root / "tone.wav";
  writeWav(path);
  std::atomic_bool cancelled{false};
  SF_INFO info{};
  std::vector<short> expected;
  require(decodeAudioToPCM(fspath_to_path_t(path), expected, info, cancelled), "legacy decode");
  audio::DecodedAssetCache cache;
  std::vector<std::future<std::shared_ptr<const audio::DecodedAssetCache::DecodedSound>>> pending;
  for (int i = 0; i < 12; ++i)
    pending.push_back(std::async(std::launch::async, [&] { return cache.Load(path, cancelled); }));
  const auto first = pending.front().get();
  require(first && first->pcm == expected && first->info.channels == 2, "whole PCM matches legacy decode");
  for (std::size_t i = 1; i < pending.size(); ++i)
    require(pending[i].get() == first, "concurrent requests share one immutable buffer");
  require(cache.GetStatistics().decoderOpens == 1 && cache.GetStatistics().liveDecoders == 0,
          "one full decode closes its decoder immediately");
  const auto gameplay = cache.Load(path, cancelled);
  require(gameplay == first && gameplay->pcm == expected,
          "gameplay reuses exact native PCM");
  require(cache.GetStatistics().decodedFrames == 12017, "gameplay must not decode again");
}
void testCancellationAndInvalidAssets(const std::filesystem::path &root) {
  const auto path = root / "cancel.wav";
  writeWav(path);
  audio::DecodedAssetCache cache;
  std::atomic_bool cancelled{false};
  const auto ready = cache.Load(path, cancelled);
  require(bool(ready), "prepare full sound");
  cancelled = true;
  audio::DecodedAssetCache::FailureKind failure;
  require(!cache.Load(path, cancelled, {}, nullptr, &failure) &&
              failure == audio::DecodedAssetCache::FailureKind::Cancelled,
          "cached requests still observe cancellation");
  require(cache.Contains(path), "cancellation retains completed PCM");
  cancelled = false;
  std::stop_source stop; stop.request_stop();
  require(!cache.Load(path, cancelled, stop.get_token()), "stop token cancels cache hits");
  require(cache.Load(path, cancelled) == ready, "next request reuses completed sound");
  require(!cache.Load(root / "missing.wav", cancelled, {}, nullptr, &failure) &&
              failure == audio::DecodedAssetCache::FailureKind::InvalidAsset, "missing asset classification");
  { std::ofstream file(root / "invalid.wav"); file << "invalid"; }
  require(!cache.Load(root / "invalid.wav", cancelled, {}, nullptr, &failure) &&
              failure == audio::DecodedAssetCache::FailureKind::InvalidAsset, "corrupt asset classification");
  const auto large = root / "large.wav";
  writeWav(large, 2, 8 * 1024 * 1024);
  auto loading = std::async(std::launch::async, [&] { return cache.Load(large, cancelled); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cache.GetStatistics().liveDecoders == 0 && loading.wait_for(std::chrono::milliseconds(0)) !=
         std::future_status::ready && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  const bool active = cache.GetStatistics().liveDecoders != 0;
  cancelled = true;
  const auto result = loading.get();
  require(active && !result && cache.GetStatistics().liveDecoders == 0,
          "cancellation during full decode discards partial PCM and closes decoder");
  require(cache.Contains(path), "cancelling another load keeps completed keysounds");
}
void testEvictionAndIdentity(const std::filesystem::path &root) {
  const auto first = root / "bounded1.wav", second = root / "bounded2.wav";
  writeWav(first, 1, 100); writeWav(second, 1, 100);
  std::atomic_bool cancelled{false};
  audio::DecodedAssetCache cache(200);
  const auto owned = cache.Load(first, cancelled);
  require(bool(owned) && bool(cache.Load(second, cancelled)), "load bounded sounds");
  require(!cache.Contains(first) && cache.Contains(second) && cache.GetStatistics().retainedBytes == 200,
          "whole-sound LRU honors byte budget");
  require(owned->pcm.size() == 100, "eviction does not invalidate an existing owner");
  writeWav(second, 2, 500);
  require(!cache.Contains(second), "source changes invalidate cache");
  require(cache.Load(second, cancelled)->pcm.size() == 1000 && !cache.Contains(second) &&
              cache.GetStatistics().retainedBytes == 0, "oversized sound loads without retention");
  audio::DecodedAssetCache unlimited(audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  auto old = unlimited.Load(first, cancelled);
  const auto timestamp = std::filesystem::last_write_time(first);
  const auto replacement = root / "replacement.wav";
  writeWav(replacement, 1, 100);
  std::filesystem::last_write_time(replacement, timestamp);
  std::filesystem::rename(replacement, first);
  require(!unlimited.Contains(first) && unlimited.Load(first, cancelled) != old,
          "inode replacement invalidates even equal size and mtime");
  old = unlimited.Load(first, cancelled);
  const auto unchangedTime = std::filesystem::last_write_time(first);
  {
    std::fstream file(first, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(-1, std::ios::end);
    const char changed = static_cast<char>(file.get() ^ 1);
    file.seekp(-1, std::ios::end);
    file.put(changed);
    require(bool(file), "rewrite PCM without replacing the inode or changing size");
  }
  std::filesystem::last_write_time(first, unchangedTime);
  require(!unlimited.Contains(first) && unlimited.Load(first, cancelled) != old,
          "change time invalidates an in-place rewrite with restored size and mtime");
  const auto link = root / "link.wav";
  std::filesystem::create_symlink(first, link);
  old = unlimited.Load(link, cancelled);
  std::filesystem::remove(link); std::filesystem::create_symlink(second, link);
  require(!unlimited.Contains(link) && unlimited.Load(link, cancelled)->info.channels == 2,
          "symlink retargeting changes source identity");
}
void testArchiveBytesAndOwnerLifetime(const std::filesystem::path &root) {
  const auto wav = root / "archive-tone.wav", zip = root / "sounds.zip";
  writeWav(wav);
  const auto bytes = bytesAt(wav);
  zipWav(zip, bytes);
  const auto logical = archive_file::makeVirtualPath(zip, "sounds/tone.wav");
  std::atomic_bool cancelled{false};
  SF_INFO info{}; std::vector<short> expected;
  require(decodeAudioBytesToPCM(fspath_to_path_t(logical), bytes, expected, info, cancelled), "legacy archive decode");
  std::shared_ptr<const audio::DecodedAssetCache::DecodedSound> survivor;
  {
    audio::DecodedAssetCache cache;
    survivor = cache.LoadBytes(logical, bytes, cancelled);
    require(survivor && survivor->pcm == expected, "batch bytes decode whole PCM directly");
    const auto gameplay = cache.Load(logical, cancelled);
    require(gameplay == survivor && gameplay->pcm == expected,
            "gameplay reuses batch-seeded native PCM");
    require(cache.GetStatistics().archiveReads == 0 && cache.GetStatistics().decoderOpens == 1,
            "batch reuse neither extracts nor decodes twice");
  }
  require(survivor->pcm == expected, "immutable PCM outlives cache");
}
void testArchiveBatchIdentityAndGameplayReuse(const std::filesystem::path &root) {
  const auto wav = root / "batch-tone.wav", zip = root / "batch.zip";
  writeWav(wav);
  const auto bytes = bytesAt(wav);
  zipWav(zip, bytes);
  const auto logical = archive_file::makeVirtualPath(zip, "sounds/tone.wav");
  std::atomic_bool cancelled{false};
  audio::DecodedAssetCache cache;
  const auto batch = cache.BeginArchiveBatch(zip);
  require(bool(batch), "begin validated archive batch");
  const auto sound = cache.LoadArchiveBytes(*batch, logical, bytes, cancelled);
  require(sound && cache.FindArchiveAsset(*batch, logical) == sound &&
              cache.ArchiveBatchUnchanged(*batch), "batch buffers share one source generation");
  const auto gameplay = cache.Load(logical, cancelled);
  require(gameplay == sound &&
              cache.GetStatistics().decoderOpens == 1,
          "batch identity must match normal gameplay cache lookup");
  const auto foreign = archive_file::makeVirtualPath(root / "different.zip", "sounds/tone.wav");
  require(!cache.LoadArchiveBytes(*batch, foreign, bytes, cancelled) &&
              !cache.FindArchiveAsset(*batch, zip / "../outside.wav"),
          "batch identity must not apply to files outside its archive");
  cancelled = true;
  require(!cache.LoadArchiveBytes(*batch, logical, bytes, cancelled), "batch cache hits observe cancellation");
  cancelled = false;
  const auto stamp = std::filesystem::last_write_time(zip);
  const auto replacement = root / "batch-replacement.zip";
  zipWav(replacement, bytes);
  std::filesystem::last_write_time(replacement, stamp);
  std::filesystem::rename(replacement, zip);
  require(!cache.ArchiveBatchUnchanged(*batch) && !cache.Contains(logical),
          "batch completion detects source replacement with unchanged size and mtime");
  const auto changed = cache.BeginArchiveBatch(zip);
  require(changed && !cache.FindArchiveAsset(*changed, logical), "next batch must reject stale native PCM");
  require(bool(cache.LoadArchiveBytes(*changed, logical, bytes, cancelled)) &&
              cache.GetStatistics().decoderOpens == 2, "new generation decodes instead of reusing stale PCM");
  std::filesystem::remove(zip);
  require(!cache.ArchiveBatchUnchanged(*changed) && !cache.BeginArchiveBatch(zip),
          "removed archives fail batch validation");
}

void testRejectsExcessiveDecodeBeforeAllocation(const std::filesystem::path &root) {
  const auto flac = root / "oversized-header.flac";
  {
    SF_INFO info{}; info.samplerate = 48000; info.channels = 2;
    info.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
    auto file = asobmashow::audio::openSoundFileHandle(flac, SFM_WRITE, info);
    require(bool(file), "create FLAC fixture");
    const short samples[32]{};
    require(sf_writef_short(file.get(), samples, 16) == 16, "write FLAC fixture");
  }
  auto bytes = bytesAt(flac);
  require(bytes.size() > 26 && bytes[4] % 128 == 0, "FLAC starts with STREAMINFO");
  // STREAMINFO's low 36 bits advertise enough stereo frames for 512 MiB PCM,
  // despite the tiny encoded file. libsndfile trusts this count on open.
  std::uint64_t packed = 0;
  for (int i = 18; i < 26; ++i) packed = (packed << 8) | bytes[i];
  packed = (packed & ~((std::uint64_t{1} << 36) - 1)) | (128U * 1024U * 1024U);
  for (int i = 25; i >= 18; --i) { bytes[i] = packed & 255; packed >>= 8; }
  { std::ofstream output(flac, std::ios::binary); output.write(
        reinterpret_cast<const char *>(bytes.data()), bytes.size()); }
  SF_INFO advertised{};
  auto header = asobmashow::audio::openSoundFileHandle(flac, SFM_READ, advertised);
  require(header && advertised.frames == 128U * 1024U * 1024U,
          "fixture must advertise the excessive PCM allocation");
  header.reset();
  const auto zip = root / "oversized-header.zip";
  zipWav(zip, bytes);
  const auto logical = archive_file::makeVirtualPath(zip, "sounds/tone.wav");
  std::atomic_bool cancelled{false};
  for (const auto retention : {audio::DecodedAssetCache::kDefaultRetainedBytes,
                               audio::DecodedAssetCache::kUnlimitedRetainedBytes}) {
    audio::DecodedAssetCache cache(retention);
    const auto batch = cache.BeginArchiveBatch(zip);
    require(bool(batch), "begin oversized fixture archive batch");
    for (int mode = 0; mode < 4; ++mode) {
      audio::DecodedAssetCache::FailureKind failure{};
      std::shared_ptr<const audio::DecodedAssetCache::DecodedSound> sound;
      std::size_t largest;
      {
        test_support::AllocationSizeObserver observer;
        test_support::AllocationSizeLimit limit(1024U * 1024U);
        if (mode == 0) sound = cache.Load(flac, cancelled, {}, nullptr, &failure);
        if (mode == 1) sound = cache.LoadBytes(logical, bytes, cancelled, {}, nullptr, &failure);
        if (mode == 2) sound = cache.LoadArchiveBytes(*batch, logical, bytes, cancelled, {}, nullptr, &failure);
        if (mode == 3) sound = cache.Load(logical, cancelled, {}, nullptr, &failure);
        largest = observer.largest();
      }
      require(!sound && failure == audio::DecodedAssetCache::FailureKind::InvalidAsset,
              "all cache decode paths must reject excessive PCM as an invalid asset");
      require(largest <= 1024U * 1024U, "reject excessive PCM before attempting its allocation");
      require(cache.GetStatistics().retainedBytes == 0 && cache.GetStatistics().liveDecoders == 0,
              "rejected assets retain no PCM or decoder");
    }
  }
  const auto oversizedFile = root / "oversized-encoded.wav";
  writeWav(oversizedFile, 1, 16);
  std::filesystem::resize_file(oversizedFile, 257U * 1024U * 1024U);
  audio::DecodedAssetCache cache;
  audio::DecodedAssetCache::FailureKind failure{};
  require(!cache.Load(oversizedFile, cancelled, {}, nullptr, &failure) &&
              failure == audio::DecodedAssetCache::FailureKind::InvalidAsset,
          "encoded file size is bounded even when its PCM payload is tiny");
}

}
int main() {
  const auto root = std::filesystem::temp_directory_path() / ("decoded-asset-tests-" + uuid::generateV4());
  try {
    std::filesystem::create_directories(root);
    testChartAudioFileLookup(root);
    testRejectsExcessiveDecodeBeforeAllocation(root);
    testWholeLoadAndConcurrentReuse(root);
    testCancellationAndInvalidAssets(root);
    testEvictionAndIdentity(root);
    testArchiveBytesAndOwnerLifetime(root);
    testArchiveBatchIdentityAndGameplayReuse(root);
    std::filesystem::remove_all(root);
    std::cout << "decoded asset cache tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::filesystem::remove_all(root);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
