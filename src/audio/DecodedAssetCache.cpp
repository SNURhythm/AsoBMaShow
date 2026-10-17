#include "DecodedAssetCache.h"

#include "../ArchiveFile.h"
#include "../ArchiveSourceIdentity.h"
#include "../RAII.h"
#include "../targets.h"
#include "ChartAssetExtensions.h"
#include "decoder.h"
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <chrono>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace audio {
namespace {
struct CacheFailure : std::runtime_error {
  DecodedAssetCache::FailureKind kind;
  CacheFailure(DecodedAssetCache::FailureKind value, const char *message)
      : std::runtime_error(message), kind(value) {}
};

bool cancelled(const std::atomic_bool &value, std::stop_token stop) {
  return value.load(std::memory_order_relaxed) || stop.stop_requested();
}
void checkpoint(const std::atomic_bool &value, std::stop_token stop) {
  if (cancelled(value, stop)) throw CacheFailure(
      DecodedAssetCache::FailureKind::Cancelled, "Audio preparation cancelled.");
}

std::filesystem::path resolvedPath(const std::filesystem::path &path) {
  static const std::vector<std::string_view> extensions(
      asobmshow::chart_assets::kAudioExtensions.begin(),
      asobmshow::chart_assets::kAudioExtensions.end());
  auto resolved = archive_file::findFileWithExtensions(path, extensions).value_or(path);
  if (!archive_file::isVirtualPath(resolved)) {
#if TARGET_OS_ANDROID
    if (IsAndroidTreePath(resolved)) return resolved.lexically_normal();
#endif
    std::error_code error;
    auto absolute = std::filesystem::absolute(resolved, error);
    if (!error) resolved = std::move(absolute);
  }
  return resolved.lexically_normal();
}

std::string fileIdentity(std::filesystem::path source) {
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(source)) {
    std::string error;
    const auto fd = OpenAndroidTreeFileDescriptor(source, error);
    if (!fd) return {};
    struct stat status{};
    const bool valid = fstat(*fd, &status) == 0 && S_ISREG(status.st_mode);
    close(*fd);
    if (!valid) return {};
    return std::to_string(status.st_dev) + ':' +
           std::to_string(status.st_ino) + ':' + std::to_string(status.st_size) + ':' +
           std::to_string(status.st_mtim.tv_sec) + ':' + std::to_string(status.st_mtim.tv_nsec) + ':' +
           std::to_string(status.st_ctim.tv_sec) + ':' + std::to_string(status.st_ctim.tv_nsec);
  }
#endif
  if (!archive_source::isReference(source)) {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(source, error);
    if (!error) source = std::move(canonical);
  }
  return archive_source_identity::KeyForPath(source);
}

std::string sourceIdentity(const std::filesystem::path &path) {
  std::filesystem::path source, inner;
  if (!archive_file::splitVirtualPath(path, source, inner)) source = path;
  const auto identity = fileIdentity(source);
  return identity.empty() ? std::string{} : fspath_to_utf8(path) + '|' + identity;
}

std::string batchAssetIdentity(const DecodedAssetCache::ArchiveBatch &batch,
                               const std::filesystem::path &path) {
  const auto inner = path.lexically_relative(batch.path);
  if (batch.identity.empty() || inner.empty() || inner == "." || inner.is_absolute() ||
      *inner.begin() == "..") return {};
  return fspath_to_utf8(path) + '|' + batch.identity;
}

} // namespace

struct DecodedAssetCache::Impl {
  struct Entry {
    std::string identity;
    std::timed_mutex decodeMutex;
    std::shared_ptr<const DecodedSound> sound; // Protected by the cache mutex.
    bool retained = false;
    std::list<std::shared_ptr<Entry>>::iterator lru;
  };
  explicit Impl(std::size_t maximum) : maximumBytes(maximum) {}
  const std::size_t maximumBytes;
  mutable std::mutex mutex;
  std::map<std::filesystem::path, std::shared_ptr<Entry>> entries;
  std::list<std::shared_ptr<Entry>> lru;
  Statistics statistics;

  void forget(Entry &entry) {
    if (!entry.retained) return;
    statistics.retainedBytes -= entry.sound->pcm.size() * sizeof(short);
    entry.sound.reset();
    lru.erase(entry.lru);
    entry.retained = false;
  }

  std::shared_ptr<const DecodedSound> cached(const std::shared_ptr<Entry> &entry) {
    std::lock_guard lock(mutex);
    return cachedLocked(entry);
  }

  std::shared_ptr<const DecodedSound> cachedLocked(const std::shared_ptr<Entry> &entry) {
    if (!entry->sound) return {};
    lru.splice(lru.end(), lru, entry->lru);
    statistics.cacheHitFrames += entry->sound->info.frames;
    return entry->sound;
  }

  std::shared_ptr<const DecodedSound> load(const std::filesystem::path &path,
      const std::vector<unsigned char> *encoded, const std::atomic_bool &cancel,
      std::stop_token stop, std::string *error, FailureKind *failure,
      const ArchiveBatch *batch = nullptr) {
    if (error) error->clear();
    if (failure) *failure = FailureKind::None;
    try {
      checkpoint(cancel, stop);
      if (encoded && encoded->size() > kMaximumEncodedAssetBytes)
        throw CacheFailure(FailureKind::InvalidAsset, "Encoded audio exceeds the byte limit.");
      const auto resolved = batch ? path.lexically_normal() : resolvedPath(path);
      const auto identity = batch ? batchAssetIdentity(*batch, resolved) : sourceIdentity(resolved);
      if (identity.empty()) throw CacheFailure(FailureKind::InvalidAsset,
                                               "Audio source is unavailable.");
      std::shared_ptr<Entry> entry;
      {
        std::lock_guard lock(mutex);
        auto &current = entries[resolved];
        if (!current || current->identity != identity) {
          if (current) forget(*current);
          current = std::make_shared<Entry>();
          current->identity = identity;
        }
        entry = current;
      }
      if (auto ready = cached(entry)) return ready;
      std::unique_lock entryLock(entry->decodeMutex, std::defer_lock);
      while (!entryLock.try_lock_for(std::chrono::milliseconds(10))) checkpoint(cancel, stop);
      checkpoint(cancel, stop);
      if (auto ready = cached(entry)) return ready;
      auto sound = std::make_shared<DecodedSound>();
      {
        std::lock_guard lock(mutex);
        ++statistics.decoderOpens;
        ++statistics.liveDecoders;
        statistics.peakLiveDecoders = std::max(statistics.peakLiveDecoders, statistics.liveDecoders);
        if (!encoded && archive_file::isVirtualPath(resolved)) ++statistics.archiveReads;
      }
      ScopeExit closed([&] {
        std::lock_guard lock(mutex);
        --statistics.liveDecoders;
      });
      // The legacy full decoder only reads this flag; its older API is non-const.
      auto &decodeCancelled = const_cast<std::atomic_bool &>(cancel);
      constexpr AudioDecodeLimits limits{
          .maximumEncodedBytes = kMaximumEncodedAssetBytes,
          .maximumPcmSamples = kMaximumPcmAssetBytes / sizeof(short)};
      std::vector<short> pcm;
      const bool decoded = encoded
          ? decodeAudioBytesToPCMBounded(fspath_to_path_t(resolved), *encoded,
                pcm, sound->info, decodeCancelled, limits.maximumPcmSamples)
          : decodeAudioToPCMBounded(fspath_to_path_t(resolved), pcm,
                                    sound->info, decodeCancelled, limits, stop);
      checkpoint(cancel, stop);
      if (!batch && sourceIdentity(resolved) != identity)
        throw std::runtime_error("Audio source changed during preparation.");
      if (!decoded || sound->info.samplerate <= 0) throw CacheFailure(FailureKind::InvalidAsset, "Could not decode audio source.");
      sound->pcm = std::move(pcm);
      const auto bytes = sound->pcm.size() * sizeof(short);
      {
        std::lock_guard lock(mutex);
        statistics.decodedFrames += sound->info.frames;
        const auto current = entries.find(resolved);
        if (current != entries.end() && current->second == entry && bytes <= maximumBytes) {
          while (statistics.retainedBytes > maximumBytes - bytes && !lru.empty())
            forget(*lru.front());
          lru.push_back(entry);
          entry->sound = sound;
          entry->lru = std::prev(lru.end());
          entry->retained = true;
          statistics.retainedBytes += bytes;
        }
      }
      return sound;
    } catch (const std::exception &exception) {
      if (error) *error = exception.what();
      if (failure) {
        const auto *classified = dynamic_cast<const CacheFailure *>(&exception);
        *failure = classified ? classified->kind : FailureKind::Operation;
      }
      return {};
    }
  }
};

DecodedAssetCache::DecodedAssetCache(std::size_t maximum)
    : impl(std::make_shared<Impl>(maximum)) {}
DecodedAssetCache::~DecodedAssetCache() = default;
std::shared_ptr<const DecodedAssetCache::DecodedSound> DecodedAssetCache::Load(
    const std::filesystem::path &path, const std::atomic_bool &cancel,
    std::stop_token stop, std::string *error, FailureKind *failure) {
  return impl->load(path, nullptr, cancel, stop, error, failure);
}
std::shared_ptr<const DecodedAssetCache::DecodedSound> DecodedAssetCache::LoadBytes(
    const std::filesystem::path &path, const std::vector<unsigned char> &bytes,
    const std::atomic_bool &cancel, std::stop_token stop, std::string *error,
    FailureKind *failure) {
  return impl->load(path, &bytes, cancel, stop, error, failure);
}
std::optional<DecodedAssetCache::ArchiveBatch> DecodedAssetCache::BeginArchiveBatch(
    const std::filesystem::path &path) const {
  auto normalized = path.lexically_normal();
  auto identity = fileIdentity(normalized);
  if (identity.empty()) return std::nullopt;
  return ArchiveBatch{std::move(normalized), std::move(identity)};
}
bool DecodedAssetCache::ArchiveBatchUnchanged(const ArchiveBatch &batch) const {
  return !batch.identity.empty() && fileIdentity(batch.path) == batch.identity;
}
std::shared_ptr<const DecodedAssetCache::DecodedSound> DecodedAssetCache::FindArchiveAsset(
    const ArchiveBatch &batch, const std::filesystem::path &path) {
  const auto resolved = path.lexically_normal();
  const auto identity = batchAssetIdentity(batch, resolved);
  std::lock_guard lock(impl->mutex);
  const auto found = impl->entries.find(resolved);
  if (identity.empty() || found == impl->entries.end() || found->second->identity != identity)
    return {};
  return impl->cachedLocked(found->second);
}
std::shared_ptr<const DecodedAssetCache::DecodedSound> DecodedAssetCache::LoadArchiveBytes(
    const ArchiveBatch &batch, const std::filesystem::path &path,
    const std::vector<unsigned char> &bytes, const std::atomic_bool &cancel,
    std::stop_token stop, std::string *error, FailureKind *failure) {
  return impl->load(path, &bytes, cancel, stop, error, failure, &batch);
}
bool DecodedAssetCache::Contains(const std::filesystem::path &path) const {
  const auto resolved = resolvedPath(path);
  const auto identity = sourceIdentity(resolved);
  std::lock_guard lock(impl->mutex);
  const auto found = impl->entries.find(resolved);
  return !identity.empty() && found != impl->entries.end() &&
      found->second->identity == identity && bool(found->second->sound);
}
void DecodedAssetCache::Clear() {
  std::lock_guard lock(impl->mutex);
  while (!impl->lru.empty()) impl->forget(*impl->lru.front());
  impl->entries.clear();
}
DecodedAssetCache::Statistics DecodedAssetCache::GetStatistics() const {
  std::lock_guard lock(impl->mutex);
  return impl->statistics;
}
} // namespace audio
