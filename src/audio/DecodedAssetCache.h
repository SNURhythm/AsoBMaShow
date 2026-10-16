#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <limits>
#include <sndfile.h>
#include <stop_token>
#include <string>
#include <vector>

namespace audio {

// Session-owned, native-rate PCM shared by preview preparation and the later
// gameplay load. All methods are loader/worker operations, never audio callbacks.
class DecodedAssetCache {
  struct Impl;
public:
  // Worker-scoped retention of a decoder cursor, independent of PCM retention.
  // Pinning performs no decoding. The token may safely outlive the cache owner.
  class DecoderPin {
  public:
    DecoderPin();
    ~DecoderPin();
    DecoderPin(DecoderPin &&) noexcept;
    DecoderPin &operator=(DecoderPin &&) noexcept;
    explicit operator bool() const noexcept;
  private:
    friend class DecodedAssetCache;
    struct State;
    std::unique_ptr<State> state;
  };
  static constexpr std::size_t kPageFrames = 4096;
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
    std::size_t pinnedEntries = 0;
  };

  explicit DecodedAssetCache(std::filesystem::path scratchRoot = {},
                            std::size_t maximumRetainedBytes = kDefaultRetainedBytes);
  ~DecodedAssetCache();
  DecodedAssetCache(const DecodedAssetCache &) = delete;
  DecodedAssetCache &operator=(const DecodedAssetCache &) = delete;

  // Unpinned active/idle decoders share a 16-descriptor bound. Explicit pins
  // preferentially retain progressively warmed sources, within a 64-decoder
  // total bound. Excess idle pinned cursors can close and reopen as needed;
  // descriptor eviction never removes retained PCM.
  DecoderPin PinDecoder(const std::filesystem::path &path,
                        const std::atomic_bool &cancelled, std::stop_token stop = {},
                        std::string *error = nullptr, FailureKind *failure = nullptr);

  bool Inspect(const std::filesystem::path &path, SF_INFO &info,
               const std::atomic_bool &cancelled, std::stop_token stop = {},
               std::string *error = nullptr, FailureKind *failure = nullptr);
  // firstFrame need not be page-aligned. Returns only frames before EOF, in
  // native channel order, with the same double-to-int16 conversion as decoder.cpp.
  bool ReadRange(const std::filesystem::path &path, std::uint64_t firstFrame,
                 std::size_t frameCount, std::vector<short> &pcm, SF_INFO &info,
                 const std::atomic_bool &cancelled, std::stop_token stop = {},
                 std::string *error = nullptr);
  bool Materialize(const std::filesystem::path &path, std::vector<short> &pcm,
                   SF_INFO &info, const std::atomic_bool &cancelled,
                   std::stop_token stop = {}, std::string *error = nullptr);
  // Warm missing native-rate pages without allocating another full PCM copy.
  bool Preload(const std::filesystem::path &path, const std::atomic_bool &cancelled,
               std::stop_token stop = {}, std::string *error = nullptr,
               FailureKind *failure = nullptr);
  // Stage encoded entries in one streaming extraction per archive, without
  // decoding PCM. Callers choose a near-future window before warming the rest.
  // Regular paths and already staged entries need no archive work.
  bool StageArchiveAssets(const std::vector<std::filesystem::path> &paths,
                          const std::atomic_bool &cancelled,
                          std::stop_token stop = {}, std::string *error = nullptr);
  // Existing batch archive extraction can seed an uncached asset without
  // extracting that same entry again. The logical original path remains its key.
  bool MaterializeBytes(const std::filesystem::path &path,
                        const std::vector<unsigned char> &encoded,
                        std::vector<short> &pcm, SF_INFO &info,
                        const std::atomic_bool &cancelled,
                        std::stop_token stop = {}, std::string *error = nullptr);
  [[nodiscard]] bool Contains(const std::filesystem::path &path) const;
  [[nodiscard]] Statistics GetStatistics() const;

private:
  std::shared_ptr<Impl> impl;
};

} // namespace audio
