#pragma once

#include "SharedPcmBuffer.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace audio {

// One producer publishes once, then fills fixed slots in any order. Consumers
// only acquire raw pointers: the owner and every published allocation must live
// until callbacks have stopped. Never replace SoundData::pagedOutput live.
class PagedPcmSound {
public:
  static constexpr std::size_t kPageFrames = 4096;
  static_assert(sizeof(short) == sizeof(std::int16_t));
  static_assert(std::atomic<const short *>::is_always_lock_free);

  struct Metadata {
    const int channels;
    const std::size_t frameCount;
    const std::size_t pageCount;

    [[nodiscard]] const short *pageSamples(std::size_t pageIndex) const noexcept {
      return pageIndex < pageCount
                 ? pages_[pageIndex].load(std::memory_order_acquire) : nullptr;
    }

  private:
    friend class PagedPcmSound;
    Metadata(int channelCount, std::size_t frames)
        : channels(channelCount), frameCount(frames),
          pageCount(frames / kPageFrames + (frames % kPageFrames != 0)),
          pages_(std::make_unique<std::atomic<const short *>[]>(pageCount)),
          owners_(pageCount) {
      for (std::size_t index = 0; index < pageCount; ++index) {
        pages_[index].store(nullptr, std::memory_order_relaxed);
      }
    }

    std::unique_ptr<std::atomic<const short *>[]> pages_;
    std::vector<std::unique_ptr<short[]>> owners_;
    SharedPcmBuffer completeBuffer_;
  };

  PagedPcmSound() = default;
  PagedPcmSound(const PagedPcmSound &) = delete;
  PagedPcmSound &operator=(const PagedPcmSound &) = delete;

  // Null means pending; zero frameCount means resolved invalid/empty audio.
  [[nodiscard]] const Metadata *metadata() const noexcept {
    return published_.load(std::memory_order_acquire);
  }

  // Producer-only. Allocation failures propagate to the preparation worker.
  // The Q32 mixer represents at most UINT32_MAX whole source frames.
  bool publishMetadata(int channels, std::size_t frameCount) {
    if (owner_ || channels < 0 || (frameCount != 0 && channels == 0) ||
        frameCount > (std::numeric_limits<std::uint64_t>::max() >> 32) ||
        (channels != 0 && frameCount > std::numeric_limits<std::size_t>::max() /
                                          static_cast<std::size_t>(channels))) {
      return false;
    }
    owner_ = std::unique_ptr<Metadata>(new Metadata(channels, frameCount));
    published_.store(owner_.get(), std::memory_order_release);
    return true;
  }

  // Publish a whole keysound with one allocation owner and no per-page copies.
  bool publishFull(int channels, SharedPcmBuffer samples) {
    if (owner_ || channels <= 0 || samples.size() % channels != 0 ||
        samples.size() / channels > UINT32_MAX) return false;
    auto ready = std::unique_ptr<Metadata>(new Metadata(channels, samples.size() / channels));
    ready->completeBuffer_ = std::move(samples);
    for (std::size_t page = 0; page < ready->pageCount; ++page)
      ready->pages_[page].store(ready->completeBuffer_.data() + page * kPageFrames * channels,
                                std::memory_order_relaxed);
    owner_ = std::move(ready);
    published_.store(owner_.get(), std::memory_order_release);
    return true;
  }

  // Copy exactly one complete (possibly final partial) interleaved output-rate
  // PCM page. Published samples are immutable and cannot be replaced.
  bool publishPage(std::size_t pageIndex, std::span<const short> samples) {
    if (!owner_ || pageIndex >= owner_->pageCount || !owner_->completeBuffer_.empty() || owner_->owners_[pageIndex]) {
      return false;
    }
    const std::size_t frames = std::min(kPageFrames,
        owner_->frameCount - pageIndex * kPageFrames);
    if (samples.size() != frames * static_cast<std::size_t>(owner_->channels)) {
      return false;
    }
    auto page = std::make_unique<short[]>(samples.size());
    std::copy(samples.begin(), samples.end(), page.get());
    owner_->owners_[pageIndex] = std::move(page);
    owner_->pages_[pageIndex].store(owner_->owners_[pageIndex].get(),
                                    std::memory_order_release);
    return true;
  }

private:
  static_assert(std::atomic<const Metadata *>::is_always_lock_free);
  std::unique_ptr<Metadata> owner_;
  std::atomic<const Metadata *> published_{nullptr};
};

} // namespace audio
