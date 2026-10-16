#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <sndfile.h>
#include <stop_token>
#include <string>
#include <vector>

namespace audio {
// Whole native-rate keysounds shared by preview and gameplay. Decode once using
// the full loader; readers retain immutable PCM independently of cache eviction.
// All operations are loader/worker operations, never audio callbacks.
class DecodedAssetCache {
  struct Impl;
public:
  struct DecodedSound {
    SF_INFO info{};
    std::vector<short> pcm;
  };
  static constexpr std::size_t kDefaultRetainedBytes = 64U * 1024U * 1024U;
  static constexpr std::size_t kUnlimitedRetainedBytes = std::numeric_limits<std::size_t>::max();
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
  // scratchRoot is retained for caller compatibility; whole-buffer loading
  // creates no scratch directory or extracted-audio files.
  explicit DecodedAssetCache(std::filesystem::path scratchRoot = {},
                            std::size_t maximumRetainedBytes = kDefaultRetainedBytes);
  ~DecodedAssetCache();
  DecodedAssetCache(const DecodedAssetCache &) = delete;
  DecodedAssetCache &operator=(const DecodedAssetCache &) = delete;
  std::shared_ptr<const DecodedSound> Load(const std::filesystem::path &path,
      const std::atomic_bool &cancelled, std::stop_token stop = {},
      std::string *error = nullptr, FailureKind *failure = nullptr);
  std::shared_ptr<const DecodedSound> LoadBytes(const std::filesystem::path &path,
      const std::vector<unsigned char> &bytes, const std::atomic_bool &cancelled,
      std::stop_token stop = {}, std::string *error = nullptr, FailureKind *failure = nullptr);
  // A batch validates its source once before resolution/extraction and again
  // before completion. Only pass fully resolved paths from that same archive.
  // This avoids filesystem/extension checks inside every decode callback.
  struct ArchiveBatch {
    std::filesystem::path path;
    std::string identity;
  };
  std::optional<ArchiveBatch> BeginArchiveBatch(const std::filesystem::path &path) const;
  bool ArchiveBatchUnchanged(const ArchiveBatch &batch) const;
  std::shared_ptr<const DecodedSound> FindArchiveAsset(const ArchiveBatch &batch,
      const std::filesystem::path &resolvedPath);
  std::shared_ptr<const DecodedSound> LoadArchiveBytes(const ArchiveBatch &batch,
      const std::filesystem::path &resolvedPath, const std::vector<unsigned char> &bytes,
      const std::atomic_bool &cancelled, std::stop_token stop = {},
      std::string *error = nullptr, FailureKind *failure = nullptr);
  bool Materialize(const std::filesystem::path &path, std::vector<short> &pcm,
      SF_INFO &info, const std::atomic_bool &cancelled,
      std::stop_token stop = {}, std::string *error = nullptr);
  bool MaterializeBytes(const std::filesystem::path &path,
      const std::vector<unsigned char> &bytes, std::vector<short> &pcm, SF_INFO &info,
      const std::atomic_bool &cancelled, std::stop_token stop = {}, std::string *error = nullptr);
  [[nodiscard]] bool Contains(const std::filesystem::path &path) const;
  [[nodiscard]] Statistics GetStatistics() const;
private:
  std::shared_ptr<Impl> impl;
};
} // namespace audio
