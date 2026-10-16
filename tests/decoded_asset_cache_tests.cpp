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
void testWholeLoadAndConcurrentReuse(const std::filesystem::path &root) {
  const auto path = root / "tone.wav";
  writeWav(path);
  std::atomic_bool cancelled{false};
  SF_INFO info{};
  std::vector<short> expected;
  require(decodeAudioToPCM(fspath_to_path_t(path), expected, info, cancelled), "legacy decode");
  audio::DecodedAssetCache cache(root / "no-scratch");
  std::vector<std::future<std::shared_ptr<const audio::DecodedAssetCache::DecodedSound>>> pending;
  for (int i = 0; i < 12; ++i)
    pending.push_back(std::async(std::launch::async, [&] { return cache.Load(path, cancelled); }));
  const auto first = pending.front().get();
  require(first && first->pcm == expected && first->info.channels == 2, "whole PCM matches legacy decode");
  for (std::size_t i = 1; i < pending.size(); ++i)
    require(pending[i].get() == first, "concurrent requests share one immutable buffer");
  require(cache.GetStatistics().decoderOpens == 1 && cache.GetStatistics().liveDecoders == 0,
          "one full decode closes its decoder immediately");
  std::vector<short> gameplay;
  require(cache.Materialize(path, gameplay, info, cancelled) && gameplay == expected,
          "gameplay reuses exact native PCM");
  require(cache.GetStatistics().decodedFrames == 12017, "gameplay must not decode again");
  require(!std::filesystem::exists(root / "no-scratch"), "whole loading creates no scratch files");
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
  audio::DecodedAssetCache cache({}, 200);
  const auto owned = cache.Load(first, cancelled);
  require(bool(owned) && bool(cache.Load(second, cancelled)), "load bounded sounds");
  require(!cache.Contains(first) && cache.Contains(second) && cache.GetStatistics().retainedBytes == 200,
          "whole-sound LRU honors byte budget");
  require(owned->pcm.size() == 100, "eviction does not invalidate an existing owner");
  writeWav(second, 2, 500);
  require(!cache.Contains(second), "source changes invalidate cache");
  require(cache.Load(second, cancelled)->pcm.size() == 1000 && !cache.Contains(second) &&
              cache.GetStatistics().retainedBytes == 0, "oversized sound loads without retention");
  audio::DecodedAssetCache unlimited({}, audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  auto old = unlimited.Load(first, cancelled);
  const auto timestamp = std::filesystem::last_write_time(first);
  const auto replacement = root / "replacement.wav";
  writeWav(replacement, 1, 100);
  std::filesystem::last_write_time(replacement, timestamp);
  std::filesystem::rename(replacement, first);
  require(!unlimited.Contains(first) && unlimited.Load(first, cancelled) != old,
          "inode replacement invalidates even equal size and mtime");
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
    audio::DecodedAssetCache cache(root / "unused-scratch");
    survivor = cache.LoadBytes(logical, bytes, cancelled);
    require(survivor && survivor->pcm == expected, "batch bytes decode whole PCM directly");
    std::vector<short> gameplay;
    require(cache.Materialize(logical, gameplay, info, cancelled) && gameplay == expected,
            "gameplay reuses batch-seeded native PCM");
    require(cache.GetStatistics().archiveReads == 0 && cache.GetStatistics().decoderOpens == 1,
            "batch reuse neither extracts nor decodes twice");
  }
  require(survivor->pcm == expected && !std::filesystem::exists(root / "unused-scratch"),
          "immutable PCM outlives cache without scratch storage");
}
}
int main() {
  const auto root = std::filesystem::temp_directory_path() / ("decoded-asset-tests-" + uuid::generateV4());
  try {
    std::filesystem::create_directories(root);
    testWholeLoadAndConcurrentReuse(root);
    testCancellationAndInvalidAssets(root);
    testEvictionAndIdentity(root);
    testArchiveBytesAndOwnerLifetime(root);
    std::filesystem::remove_all(root);
    std::cout << "decoded asset cache tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::filesystem::remove_all(root);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
