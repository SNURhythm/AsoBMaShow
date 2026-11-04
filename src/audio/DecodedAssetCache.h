#pragma once

#include "SharedPcmBuffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sndfile.h>
#include <stop_token>
#include <string>
#include <vector>

namespace audio {
// Whole keysounds shared by preview and gameplay. Decode once using
// the full loader; readers retain immutable PCM independently of cache eviction.
// All operations are loader/worker operations, never audio callbacks.
class DecodedAssetCache {
  struct Impl;
public:
  struct DecodedSound {
    SF_INFO info{};
    bool originalPcm = true;
    std::size_t durationFrames = 0;
    int durationSampleRate = 0;
    SharedPcmBuffer pcm;

    // Reuse the preview conversion while its playback owner is alive. After
    // publication, the cache replaces the original with this prepared output.
    [[nodiscard]] SharedPcmBuffer FindPreparedOutput(int sampleRate) const {
      if (sampleRate == info.samplerate) return pcm;
      std::lock_guard lock(outputMutex_);
      return sampleRate == outputRate_ ? SharedPcmBuffer(output_.lock()) : SharedPcmBuffer{};
    }
    void RememberPreparedOutput(int sampleRate, const SharedPcmBuffer &output) const {
      std::lock_guard lock(outputMutex_);
      outputRate_ = sampleRate;
      output_ = output.storage();
    }

    [[nodiscard]] std::shared_ptr<const DecodedSound> PreparedSnapshot() const {
      std::lock_guard lock(outputMutex_);
      auto output = output_.lock();
      if (!output || outputRate_ <= 0 || info.channels <= 0) return {};
      auto snapshot = std::make_shared<DecodedSound>();
      snapshot->pcm = outputRate_ == info.samplerate ? pcm : SharedPcmBuffer(std::move(output));
      snapshot->info = info;
      snapshot->info.samplerate = outputRate_;
      snapshot->info.frames = snapshot->pcm.size() / info.channels;
      snapshot->originalPcm = originalPcm && outputRate_ == info.samplerate;
      snapshot->durationFrames = durationSampleRate > 0 ? durationFrames : pcm.size() / info.channels;
      snapshot->durationSampleRate = durationSampleRate > 0 ? durationSampleRate : info.samplerate;
      return snapshot;
    }

  private:
    mutable std::mutex outputMutex_;
    mutable int outputRate_ = 0;
    mutable std::weak_ptr<const std::vector<short>> output_;
  };
  static constexpr std::size_t kDefaultRetainedBytes = 64U * 1024U * 1024U;
  static constexpr std::size_t kUnlimitedRetainedBytes = std::numeric_limits<std::size_t>::max();
  // Per-asset decode bounds are independent of cache retention, including an
  // unlimited preview cache. Validate these before allocating whole buffers.
  static constexpr std::size_t kMaximumEncodedAssetBytes = 256U * 1024U * 1024U;
  static constexpr std::size_t kMaximumPcmAssetBytes = 256U * 1024U * 1024U;
  enum class FailureKind { None, Cancelled, InvalidAsset, Operation };
  struct Statistics {
    std::uint64_t decodedFrames = 0;
    std::uint64_t cacheHitFrames = 0;
    std::size_t retainedBytes = 0;
    std::uint64_t decoderOpens = 0;
    std::uint64_t archiveReads = 0;
    std::size_t liveDecoders = 0;
    std::size_t peakLiveDecoders = 0;
  };
  explicit DecodedAssetCache(std::size_t maximumRetainedBytes = kDefaultRetainedBytes);
  ~DecodedAssetCache();
  DecodedAssetCache(const DecodedAssetCache &) = delete;
  DecodedAssetCache &operator=(const DecodedAssetCache &) = delete;
  std::shared_ptr<const DecodedSound> Load(const std::filesystem::path &path,
      const std::atomic_bool &cancelled, std::stop_token stop = {},
      std::string *error = nullptr, FailureKind *failure = nullptr, bool requireOriginal = false);
  std::shared_ptr<const DecodedSound> LoadBytes(const std::filesystem::path &path,
      const std::vector<unsigned char> &bytes, const std::atomic_bool &cancelled,
      std::stop_token stop = {}, std::string *error = nullptr, FailureKind *failure = nullptr, bool requireOriginal = false);
  // Identity-checked lookup only; never opens a decoder or reads asset payloads.
  std::shared_ptr<const DecodedSound> Find(const std::filesystem::path &path);
  // Metadata identity of a regular file or an archive container.
  struct SourceSnapshot {
    std::filesystem::path path;
    std::string identity;
  };
  using ArchiveBatch = SourceSnapshot;
  std::optional<SourceSnapshot> CaptureSource(const std::filesystem::path &path) const;
  // Seed a fresh preview cache with immutable PCM already owned by gameplay.
  // Capture source before validating the retained sound against that source.
  // Archive paths must be resolved entries of source.path; no payload is read.
  void RememberLoadedSound(const SourceSnapshot &source, const std::filesystem::path &path,
                           std::shared_ptr<const DecodedSound> sound);
  // Validate once before archive resolution/extraction and again at completion;
  // entry operations accept fully resolved paths from that same archive.
  std::optional<ArchiveBatch> BeginArchiveBatch(const std::filesystem::path &path) const;
  bool ArchiveBatchUnchanged(const ArchiveBatch &batch) const;
  std::shared_ptr<const DecodedSound> FindArchiveAsset(const ArchiveBatch &batch,
      const std::filesystem::path &resolvedPath);
  std::shared_ptr<const DecodedSound> LoadArchiveBytes(const ArchiveBatch &batch,
      const std::filesystem::path &resolvedPath, const std::vector<unsigned char> &bytes,
      const std::atomic_bool &cancelled, std::stop_token stop = {},
      std::string *error = nullptr, FailureKind *failure = nullptr, bool requireOriginal = false);
  [[nodiscard]] bool Contains(const std::filesystem::path &path) const;
  // Replace one entry with its ready output, or evict it if only dependent
  // slices were prepared. Existing readers may finish using the old snapshot.
  void ReleaseNativePcm(const std::filesystem::path &path,
                        const std::shared_ptr<const DecodedSound> &sound);
  // Release cache ownership after handoff; active playback keeps shared PCM.
  void Clear();
  [[nodiscard]] Statistics GetStatistics() const;
private:
  std::shared_ptr<Impl> impl;
};
} // namespace audio
