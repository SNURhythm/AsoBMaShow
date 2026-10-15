#include "DecodedAssetCache.h"

#include "../ArchiveFile.h"
#include "../ArchiveSourceIdentity.h"
#include "../Uuid.h"
#include "../targets.h"
#include "ChartAssetExtensions.h"
#include "SoundFileIO.h"
#include "decoder.h"
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace audio {
namespace {
constexpr std::size_t kMaximumEncodedBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumUnpinnedDecoders = 16;
constexpr std::size_t kMaximumDecoders = 64;

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

std::string sourceIdentity(const std::filesystem::path &path) {
  std::filesystem::path source, inner;
  if (!archive_file::splitVirtualPath(path, source, inner)) source = path;
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(source)) {
    std::string error;
    const auto fd = OpenAndroidTreeFileDescriptor(source, error);
    if (!fd) return {};
    struct stat status{};
    const bool valid = fstat(*fd, &status) == 0 && S_ISREG(status.st_mode);
    close(*fd);
    if (!valid) return {};
    return fspath_to_utf8(path) + ':' + std::to_string(status.st_dev) + ':' +
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
  const auto identity = archive_source_identity::KeyForPath(source);
  return identity.empty() ? std::string{} : fspath_to_utf8(path) + '|' + identity;
}

asobmashow::audio::SoundFileHandle openFile(const std::filesystem::path &path, SF_INFO &info) {
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    std::string error;
    const auto fd = OpenAndroidTreeFileDescriptor(path, error);
    if (!fd) throw std::runtime_error("Could not open Android audio source: " + error);
    asobmashow::audio::SoundFileHandle file(sf_open_fd(*fd, SFM_READ, &info, SF_TRUE));
    if (!file) close(*fd);
    return file;
  }
#endif
  return asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
}

bool sameFormat(const SF_INFO &a, const SF_INFO &b) {
  return a.frames == b.frames && a.channels == b.channels &&
         a.samplerate == b.samplerate && a.format == b.format;
}
} // namespace

struct DecodedAssetCache::Impl {
  struct Entry {
    std::uint64_t id = 0;
    std::filesystem::path logicalPath;
    std::string identity;
    std::timed_mutex mutex;
    std::filesystem::path decodedPath;
    SF_INFO info{};
    bool inspected = false;
    // Guarded by the cache mutex, not the per-entry decode mutex.
    std::size_t pins = 0;
    bool decoderReserved = false;
  };
  using PageKey = std::pair<std::uint64_t, std::uint64_t>;
  struct Page {
    std::shared_ptr<const std::vector<short>> samples;
    std::list<PageKey>::iterator lru;
  };
  struct IdleDecoder {
    Entry *entry = nullptr;
    asobmashow::audio::SoundFileHandle file;
    sf_count_t position = 0;
    std::list<std::uint64_t>::iterator lru;
  };

  std::filesystem::path scratchDirectory;
  const std::size_t maximumRetainedBytes;
  mutable std::mutex mutex;
  std::map<std::filesystem::path, std::shared_ptr<Entry>> entries;
  std::map<PageKey, Page> pages;
  std::list<PageKey> lru;
  std::uint64_t nextId = 0;
  Statistics statistics;
  // All leases (including reservations still opening) and idle decoders share
  // the hard descriptor bound. Pins have priority over the smaller unpinned
  // budget, but idle pinned cursors can be evicted at the hard limit. Idle states
  // belong exclusively to this mutex, never an entry lock, so eviction cannot
  // invert the entry -> cache lock order used by readers.
  std::map<std::uint64_t, IdleDecoder> idleDecoders;
  std::list<std::uint64_t> decoderLru;
  std::list<std::uint64_t> pinnedDecoderLru;
  std::size_t decoderSlots = 0;
  std::size_t pinnedDecoderSlots = 0;
  std::condition_variable decoderChanged;

  Impl(std::filesystem::path root, std::size_t maximum)
      : maximumRetainedBytes(maximum) {
    if (root.empty()) {
#if TARGET_OS_ANDROID
      root = GetAndroidCacheDir();
      if (root.empty()) throw std::runtime_error("Android audio cache is unavailable.");
#else
      root = std::filesystem::temp_directory_path();
#endif
    }
    scratchDirectory = root / ("decoded-assets-" + uuid::generateV4());
  }
  ~Impl() {
    idleDecoders.clear();
    std::error_code ignored;
    std::filesystem::remove_all(scratchDirectory, ignored);
  }

  // Caller owns the cache mutex. Close before releasing the reservation.
  void discardIdle(std::map<std::uint64_t, IdleDecoder>::iterator decoder) {
    auto &entry = *decoder->second.entry;
    if (entry.pins) {
      pinnedDecoderLru.erase(decoder->second.lru);
      --pinnedDecoderSlots;
    } else {
      decoderLru.erase(decoder->second.lru);
    }
    entry.decoderReserved = false;
    idleDecoders.erase(decoder);
    --decoderSlots;
    --statistics.liveDecoders;
  }

  void pin(Entry &entry) {
    std::lock_guard lock(mutex);
    if (entry.pins++ != 0) return;
    ++statistics.pinnedEntries;
    if (entry.decoderReserved) ++pinnedDecoderSlots;
    if (const auto idle = idleDecoders.find(entry.id); idle != idleDecoders.end())
      pinnedDecoderLru.splice(pinnedDecoderLru.end(), decoderLru, idle->second.lru);
    decoderChanged.notify_all();
  }

  void unpin(Entry &entry) noexcept {
    std::lock_guard lock(mutex);
    if (--entry.pins != 0) return;
    --statistics.pinnedEntries;
    if (entry.decoderReserved) --pinnedDecoderSlots;
    if (const auto idle = idleDecoders.find(entry.id); idle != idleDecoders.end())
      decoderLru.splice(decoderLru.end(), pinnedDecoderLru, idle->second.lru);
    while (decoderSlots - pinnedDecoderSlots > kMaximumUnpinnedDecoders && !decoderLru.empty())
      discardIdle(idleDecoders.find(decoderLru.front()));
    // A concurrently borrowed lease can finish; its return also trims excess.
    decoderChanged.notify_all();
  }

  std::shared_ptr<Entry> entryFor(const std::filesystem::path &path,
                                const std::atomic_bool &cancel, std::stop_token stop) {
    checkpoint(cancel, stop);
    const auto resolved = resolvedPath(path);
    const auto identity = sourceIdentity(resolved);
    checkpoint(cancel, stop);
    if (identity.empty()) throw CacheFailure(
        FailureKind::InvalidAsset, "Audio source is unavailable.");
    std::lock_guard lock(mutex);
    auto &entry = entries[resolved];
    if (entry && entry->identity == identity) return entry;
    if (entry) {
      if (const auto decoder = idleDecoders.find(entry->id); decoder != idleDecoders.end()) {
        discardIdle(decoder);
        decoderChanged.notify_all();
      }
      for (auto it = pages.begin(); it != pages.end();) {
        if (it->first.first != entry->id) { ++it; continue; }
        statistics.retainedBytes -= it->second.samples->size() * sizeof(short);
        lru.erase(it->second.lru);
        it = pages.erase(it);
      }
    }
    entry = std::make_shared<Entry>();
    entry->id = ++nextId;
    entry->logicalPath = resolved;
    entry->identity = identity;
    return entry;
  }

  std::unique_lock<std::timed_mutex> lockEntry(Entry &entry,
      const std::atomic_bool &cancel, std::stop_token stop) {
    std::unique_lock lock(entry.mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(10))) checkpoint(cancel, stop);
    checkpoint(cancel, stop);
    return lock;
  }

  // Caller owns the entry lock. Commit the path only after all encoded bytes
  // are staged and their source identity is still current.
  void stageEncoded(Entry &entry, const std::vector<unsigned char> &encoded,
                    const std::atomic_bool &cancel, std::stop_token stop) {
    checkpoint(cancel, stop);
    std::filesystem::create_directories(scratchDirectory);
    const auto staged = scratchDirectory / std::to_string(entry.id);
    ScopeExit cleanup([&] {
      std::error_code ignored;
      std::filesystem::remove(staged, ignored);
    });
    std::ofstream output(staged, std::ios::binary | std::ios::trunc);
    std::size_t offset = 0;
    while (offset < encoded.size()) {
      checkpoint(cancel, stop);
      const auto count = std::min<std::size_t>(65536, encoded.size() - offset);
      output.write(reinterpret_cast<const char *>(encoded.data() + offset), count);
      if (!output) throw std::runtime_error("Could not stage archived audio.");
      offset += count;
    }
    output.close();
    if (!output) throw std::runtime_error("Could not stage archived audio.");
    checkpoint(cancel, stop);
    if (sourceIdentity(entry.logicalPath) != entry.identity)
      throw std::runtime_error("Audio source changed during preparation.");
    entry.decodedPath = staged;
    cleanup.dismiss();
  }

  void stageArchives(const std::vector<std::filesystem::path> &paths,
                     const std::atomic_bool &cancel, std::stop_token stop) {
    using Requests = std::map<std::filesystem::path, std::shared_ptr<Entry>>;
    std::map<std::filesystem::path, Requests> archives;
    for (const auto &path : paths) {
      checkpoint(cancel, stop);
      std::filesystem::path archive, inner;
      if (!archive_file::splitVirtualPath(path, archive, inner)) continue;
      auto entry = entryFor(path, cancel, stop);
      if (archive_file::splitVirtualPath(entry->logicalPath, archive, inner))
        archives[archive].emplace(inner, std::move(entry));
    }
    for (auto &[archive, requests] : archives) {
      // The extraction backend can hold its own archive mutex in callbacks.
      // Acquire entries first, in one stable order, so a concurrent per-entry
      // reader cannot hold an entry while waiting on this batch's archive lock.
      std::vector<std::shared_ptr<Entry>> ordered;
      for (const auto &[path, entry] : requests) ordered.push_back(entry);
      std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return a->id < b->id;
      });
      std::vector<std::unique_lock<std::timed_mutex>> locks;
      locks.reserve(ordered.size());
      for (const auto &entry : ordered) locks.push_back(lockEntry(*entry, cancel, stop));
      std::erase_if(requests, [](const auto &request) {
        return !request.second->decodedPath.empty();
      });
      if (requests.empty()) continue;
      std::vector<std::filesystem::path> innerPaths;
      for (const auto &[path, entry] : requests) innerPaths.push_back(path);
      std::string error;
      std::exception_ptr failure;
      { std::lock_guard lock(mutex); ++statistics.archiveReads; }
      const bool read = archive_file::readArchiveEntriesStreamingBounded(
          archive, innerPaths, [&](archive_file::FileData &&file) {
            // Exceptions must not cross an archive SDK callback boundary.
            try {
              checkpoint(cancel, stop);
              const auto found = requests.find(file.path);
              if (found != requests.end()) stageEncoded(*found->second, file.bytes, cancel, stop);
              return true;
            } catch (...) {
              failure = std::current_exception();
              return false;
            }
          }, kMaximumEncodedBytes, &error, [&] { return !cancelled(cancel, stop); });
      if (failure) std::rethrow_exception(failure);
      checkpoint(cancel, stop);
      if (!read) throw std::runtime_error("Could not extract preview audio: " + error);
      for (const auto &[path, entry] : requests) {
        if (entry->decodedPath.empty())
          throw std::runtime_error("Could not stage requested preview audio.");
      }
    }
  }

  // A request exclusively borrows its decoder/cursor while holding the entry
  // lock. Successful leases return to their pinned/unpinned idle LRU; failed/cancelled
  // reads discard their cursor without invalidating immutable ready PCM pages.
  struct Decoder {
    Impl &owner;
    Entry *entry = nullptr;
    asobmashow::audio::SoundFileHandle file;
    sf_count_t position = 0;
    bool slot = false;
    bool reusable = false;
    explicit Decoder(Impl &value) : owner(value) {}
    ~Decoder() {
      if (!slot) return;
      const bool opened = bool(file);
      if (reusable && opened) {
        try {
          std::lock_guard lock(owner.mutex);
          const auto current = owner.entries.find(entry->logicalPath);
          if (current != owner.entries.end() && current->second.get() == entry &&
              (entry->pins || owner.decoderSlots - owner.pinnedDecoderSlots <= kMaximumUnpinnedDecoders)) {
            const auto [cached, inserted] = owner.idleDecoders.try_emplace(entry->id);
            if (inserted) {
              auto &list = entry->pins ? owner.pinnedDecoderLru : owner.decoderLru;
              try {
                list.push_back(entry->id);
              } catch (...) {
                owner.idleDecoders.erase(cached);
                throw;
              }
              cached->second.entry = entry;
              cached->second.lru = std::prev(list.end());
              cached->second.position = position;
              cached->second.file = std::move(file);
              owner.decoderChanged.notify_one();
              return;
            }
          }
        } catch (...) {
          // Returning a lease must remain noexcept, including allocation failure.
        }
      }
      file.reset();
      {
        std::lock_guard lock(owner.mutex);
        if (opened) --owner.statistics.liveDecoders;
        if (entry->pins) --owner.pinnedDecoderSlots;
        entry->decoderReserved = false;
        --owner.decoderSlots;
      }
      owner.decoderChanged.notify_all();
    }
    void borrow(Entry &value, const std::atomic_bool &cancel, std::stop_token stop) {
      {
        std::unique_lock lock(owner.mutex);
        for (;;) {
          checkpoint(cancel, stop);
          if (const auto cached = owner.idleDecoders.find(value.id);
              cached != owner.idleDecoders.end()) {
            file = std::move(cached->second.file);
            position = cached->second.position;
            auto &list = value.pins ? owner.pinnedDecoderLru : owner.decoderLru;
            list.erase(cached->second.lru);
            owner.idleDecoders.erase(cached);
            break;
          }
          const bool unpinnedSlotAvailable = value.pins ||
              owner.decoderSlots - owner.pinnedDecoderSlots < kMaximumUnpinnedDecoders;
          if (owner.decoderSlots < kMaximumDecoders && unpinnedSlotAvailable) {
            ++owner.decoderSlots;
            if (value.pins) ++owner.pinnedDecoderSlots;
            break;
          }
          if (!owner.decoderLru.empty()) {
            owner.discardIdle(owner.idleDecoders.find(owner.decoderLru.front()));
            continue;
          }
          if (owner.decoderSlots >= kMaximumDecoders && unpinnedSlotAvailable &&
              !owner.pinnedDecoderLru.empty()) {
            owner.discardIdle(owner.idleDecoders.find(owner.pinnedDecoderLru.front()));
            continue;
          }
          // Only idle cursors may be closed. Borrowed readers release their
          // reservation on return; poll cancellation while waiting for a slot.
          owner.decoderChanged.wait_for(lock, std::chrono::milliseconds(10));
        }
        entry = &value;
        value.decoderReserved = true;
        slot = true;
      }
    }
    void open(Entry &entry, const std::atomic_bool &cancel, std::stop_token stop,
              const std::vector<unsigned char> *encoded) {
      if (file) return;
      borrow(entry, cancel, stop);
      checkpoint(cancel, stop);
      if (file) return;
      if (entry.decodedPath.empty()) {
        std::filesystem::path archive, inner;
        if (archive_file::splitVirtualPath(entry.logicalPath, archive, inner)) {
          std::vector<unsigned char> extracted;
          if (encoded == nullptr) {
            std::string error;
            { std::lock_guard lock(owner.mutex); ++owner.statistics.archiveReads; }
            if (!archive_file::readFileBoundedWithCheckpoint(entry.logicalPath,
                    extracted, kMaximumEncodedBytes, &error, stop,
                    [&] { return !cancelled(cancel, stop); })) {
              checkpoint(cancel, stop);
              throw std::runtime_error("Could not extract audio: " + error);
            }
            encoded = &extracted;
          }
          owner.stageEncoded(entry, *encoded, cancel, stop);
        } else {
          entry.decodedPath = entry.logicalPath;
        }
      }
      SF_INFO opened{};
      file = openFile(entry.decodedPath, opened);
      if (file) {
        std::lock_guard lock(owner.mutex);
        ++owner.statistics.decoderOpens;
        ++owner.statistics.liveDecoders;
        owner.statistics.peakLiveDecoders = std::max(owner.statistics.peakLiveDecoders,
                                                    owner.statistics.liveDecoders);
      }
      checkpoint(cancel, stop);
      if (!file && sf_error(nullptr) == SF_ERR_SYSTEM)
        throw std::runtime_error("Could not read audio source.");
      if (!file || opened.frames < 0 || opened.channels <= 0 ||
          opened.channels > 64 || opened.samplerate <= 0)
        throw CacheFailure(FailureKind::InvalidAsset, "Could not decode audio source.");
      if (entry.inspected && !sameFormat(entry.info, opened))
        throw std::runtime_error("Audio source changed during preparation.");
      if (sourceIdentity(entry.logicalPath) != entry.identity)
        throw std::runtime_error("Audio source changed during preparation.");
      entry.info = opened;
      entry.inspected = true;
    }
  };

  std::shared_ptr<const std::vector<short>> readPage(Entry &entry, Decoder &decoder,
      std::uint64_t index, const std::atomic_bool &cancel, std::stop_token stop,
      const std::vector<unsigned char> *encoded, bool allowEviction) {
    checkpoint(cancel, stop);
    const PageKey key{entry.id, index};
    {
      std::lock_guard lock(mutex);
      if (const auto found = pages.find(key); found != pages.end()) {
        lru.splice(lru.end(), lru, found->second.lru);
        statistics.cacheHitFrames += found->second.samples->size() / entry.info.channels;
        return found->second.samples;
      }
    }
    decoder.open(entry, cancel, stop, encoded);
    const auto first = static_cast<sf_count_t>(index * kPageFrames);
    const auto frames = std::min<sf_count_t>(kPageFrames, entry.info.frames - first);
    if (frames <= 0) return std::make_shared<const std::vector<short>>();
    if (decoder.position != first && sf_seek(decoder.file.get(), first, SEEK_SET) != first)
      throw std::runtime_error("Could not seek audio source.");
    const auto samples = static_cast<std::size_t>(frames) * entry.info.channels;
    std::vector<double> scratch(samples);
    const auto read = sf_readf_double(decoder.file.get(), scratch.data(), frames);
    checkpoint(cancel, stop);
    if (read < 0) throw std::runtime_error("Could not read audio source.");
    decoder.position = first + read;
    auto pcm = std::make_shared<std::vector<short>>(samples, 0);
    std::transform(scratch.begin(), scratch.begin() + read * entry.info.channels,
                   pcm->begin(), audioSampleToPcm16);
    // Like the full decoder, a truncated source contributes silence after the
    // last readable frame. Never expose a partly filled output vector as success.
    const auto bytes = pcm->size() * sizeof(short);
    std::lock_guard lock(mutex);
    statistics.decodedFrames += frames;
    if (bytes <= maximumRetainedBytes && entries[entry.logicalPath].get() == &entry &&
        (allowEviction || statistics.retainedBytes <= maximumRetainedBytes - bytes)) {
      // A full gameplay load must not evict a late preview page before it
      // reaches that page. Admit missing pages only into remaining budget.
      while (statistics.retainedBytes > maximumRetainedBytes - bytes && !lru.empty()) {
        const auto old = pages.find(lru.front());
        statistics.retainedBytes -= old->second.samples->size() * sizeof(short);
        pages.erase(old);
        lru.pop_front();
      }
      lru.push_back(key);
      pages.emplace(key, Page{pcm, std::prev(lru.end())});
      statistics.retainedBytes += bytes;
    }
    return pcm;
  }

  bool read(const std::filesystem::path &path, std::uint64_t firstFrame,
            std::size_t requestedFrames, bool full, std::vector<short> &pcm,
            SF_INFO &info, const std::atomic_bool &cancel, std::stop_token stop,
            std::string *error, const std::vector<unsigned char> *encoded = nullptr,
            FailureKind *failure = nullptr, bool copyPcm = true) {
    if (failure) *failure = FailureKind::None;
    pcm.clear();
    info = {};
    if (error) error->clear();
    try {
      auto entry = entryFor(path, cancel, stop);
      auto entryLock = lockEntry(*entry, cancel, stop);
      Decoder decoder(*this);
      if (!entry->inspected) decoder.open(*entry, cancel, stop, encoded);
      info = entry->info;
      const auto available = static_cast<std::uint64_t>(info.frames);
      const auto frames = firstFrame >= available ? 0 :
          std::min<std::uint64_t>(available - firstFrame,
              full ? available : requestedFrames);
      if (frames > std::numeric_limits<std::size_t>::max() / info.channels)
        throw std::runtime_error("Decoded audio is too large.");
      if (copyPcm) pcm.resize(static_cast<std::size_t>(frames) * info.channels);
      std::uint64_t copied = 0;
      while (copied < frames) {
        const auto frame = firstFrame + copied;
        const auto pageIndex = frame / kPageFrames;
        const auto offset = static_cast<std::size_t>(frame % kPageFrames);
        const auto page = readPage(*entry, decoder, pageIndex, cancel, stop, encoded, !full);
        const auto count = std::min<std::uint64_t>(frames - copied,
            page->size() / info.channels - offset);
        if (copyPcm)
          std::copy_n(page->data() + offset * info.channels, count * info.channels,
                       pcm.data() + copied * info.channels);
        copied += count;
      }
      checkpoint(cancel, stop);
      if (sourceIdentity(entry->logicalPath) != entry->identity)
        throw std::runtime_error("Audio source changed during preparation.");
      decoder.reusable = true;
      return true;
    } catch (const std::exception &exception) {
      pcm.clear();
      if (error) *error = exception.what();
      if (failure) {
        const auto *classified = dynamic_cast<const CacheFailure *>(&exception);
        *failure = classified ? classified->kind : FailureKind::Operation;
      }
      return false;
    }
  }
};

struct DecodedAssetCache::DecoderPin::State {
  std::shared_ptr<Impl> owner;
  std::shared_ptr<Impl::Entry> entry;
  bool active = false;
  ~State() { if (active) owner->unpin(*entry); }
};
DecodedAssetCache::DecoderPin::DecoderPin() = default;
DecodedAssetCache::DecoderPin::~DecoderPin() = default;
DecodedAssetCache::DecoderPin::DecoderPin(DecoderPin &&) noexcept = default;
DecodedAssetCache::DecoderPin &DecodedAssetCache::DecoderPin::operator=(DecoderPin &&) noexcept = default;
DecodedAssetCache::DecoderPin::operator bool() const noexcept { return bool(state); }

DecodedAssetCache::DecodedAssetCache(std::filesystem::path root, std::size_t maximum)
    : impl(std::make_shared<Impl>(std::move(root), maximum)) {}
DecodedAssetCache::~DecodedAssetCache() = default;

DecodedAssetCache::DecoderPin DecodedAssetCache::PinDecoder(
    const std::filesystem::path &path, const std::atomic_bool &cancel,
    std::stop_token stop, std::string *error, FailureKind *failure) {
  if (error) error->clear();
  if (failure) *failure = FailureKind::None;
  DecoderPin pin;
  try {
    auto entry = impl->entryFor(path, cancel, stop);
    auto state = std::make_unique<DecoderPin::State>();
    state->owner = impl;
    state->entry = std::move(entry);
    impl->pin(*state->entry);
    state->active = true;
    pin.state = std::move(state);
  } catch (const std::exception &exception) {
    if (error) *error = exception.what();
    if (failure) {
      const auto *classified = dynamic_cast<const CacheFailure *>(&exception);
      *failure = classified ? classified->kind : FailureKind::Operation;
    }
  }
  return pin;
}

bool DecodedAssetCache::Inspect(const std::filesystem::path &path, SF_INFO &info,
    const std::atomic_bool &cancel, std::stop_token stop, std::string *error,
    FailureKind *failure) {
  std::vector<short> ignored;
  return impl->read(path, 0, 0, false, ignored, info, cancel, stop, error, nullptr, failure);
}
bool DecodedAssetCache::ReadRange(const std::filesystem::path &path,
    std::uint64_t first, std::size_t frames, std::vector<short> &pcm, SF_INFO &info,
    const std::atomic_bool &cancel, std::stop_token stop, std::string *error) {
  return impl->read(path, first, frames, false, pcm, info, cancel, stop, error);
}
bool DecodedAssetCache::Materialize(const std::filesystem::path &path,
    std::vector<short> &pcm, SF_INFO &info, const std::atomic_bool &cancel,
    std::stop_token stop, std::string *error) {
  return impl->read(path, 0, 0, true, pcm, info, cancel, stop, error);
}
bool DecodedAssetCache::Preload(const std::filesystem::path &path,
    const std::atomic_bool &cancel, std::stop_token stop, std::string *error,
    FailureKind *failure) {
  std::vector<short> ignored;
  SF_INFO info{};
  return impl->read(path, 0, 0, true, ignored, info, cancel, stop, error,
                    nullptr, failure, false);
}
bool DecodedAssetCache::StageArchiveAssets(const std::vector<std::filesystem::path> &paths,
    const std::atomic_bool &cancel, std::stop_token stop, std::string *error) {
  if (error) error->clear();
  try {
    impl->stageArchives(paths, cancel, stop);
    return true;
  } catch (const std::exception &exception) {
    if (error) *error = exception.what();
    return false;
  }
}
bool DecodedAssetCache::MaterializeBytes(const std::filesystem::path &path,
    const std::vector<unsigned char> &encoded, std::vector<short> &pcm, SF_INFO &info,
    const std::atomic_bool &cancel, std::stop_token stop, std::string *error) {
  return impl->read(path, 0, 0, true, pcm, info, cancel, stop, error, &encoded);
}
bool DecodedAssetCache::Contains(const std::filesystem::path &path) const {
  const auto resolved = resolvedPath(path);
  const auto identity = sourceIdentity(resolved);
  if (identity.empty()) return false;
  std::shared_ptr<Impl::Entry> entry;
  {
    std::lock_guard lock(impl->mutex);
    const auto found = impl->entries.find(resolved);
    if (found == impl->entries.end() || found->second->identity != identity) return false;
    entry = found->second;
  }
  std::unique_lock lock(entry->mutex, std::try_to_lock);
  return lock.owns_lock() && entry->inspected;
}
DecodedAssetCache::Statistics DecodedAssetCache::GetStatistics() const {
  std::lock_guard lock(impl->mutex);
  return impl->statistics;
}

} // namespace audio
