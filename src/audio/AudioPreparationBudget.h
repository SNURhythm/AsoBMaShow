#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stop_token>

namespace audio {

// Loader-only admission budget for transient decode/conversion working sets.
// Playback/cache retention and encoded archive queues have separate budgets.
// One asset may exceed this scheduling budget, but then runs alone.
class AudioPreparationBudget {
public:
  static constexpr std::size_t kDefaultBytes = 128U * 1024U * 1024U;
  explicit AudioPreparationBudget(std::size_t bytes = kDefaultBytes)
      : maximum_(std::max<std::size_t>(1, bytes)) {}

  static AudioPreparationBudget &Shared() {
    static AudioPreparationBudget budget;
    return budget;
  }

  class Reservation {
  public:
    explicit Reservation(AudioPreparationBudget &budget) : budget_(budget) {}
    ~Reservation() { release(); }
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;

    bool acquire(std::size_t bytes, const std::atomic_bool &cancelled,
                 std::stop_token stop = {}) {
      if (cancelled || stop.stop_requested()) return false;
      if (bytes_) return true;
      const auto requested = std::clamp<std::size_t>(bytes, 1, budget_.maximum_);
      std::unique_lock lock(budget_.mutex_);
      while (requested > budget_.maximum_ - budget_.used_) {
        if (cancelled || stop.stop_requested()) return false;
        budget_.changed_.wait_for(lock, std::chrono::milliseconds(10));
      }
      if (cancelled || stop.stop_requested()) return false;
      budget_.used_ += requested;
      bytes_ = requested;
      return true;
    }

    void release() {
      if (!bytes_) return;
      {
        std::lock_guard lock(budget_.mutex_);
        budget_.used_ -= bytes_;
        bytes_ = 0;
      }
      budget_.changed_.notify_all();
    }

  private:
    AudioPreparationBudget &budget_;
    std::size_t bytes_ = 0;
  };

private:
  const std::size_t maximum_;
  std::size_t used_ = 0;
  std::mutex mutex_;
  std::condition_variable changed_;
};

// A synchronous source load owns one reservation through decode, every BMSON
// slice conversion, and native-buffer release. Nested decoder/slice calls reuse
// that reservation instead of waiting for more budget while already holding it.
// This context must stay on its worker thread and never cross an async boundary.
class AudioPreparationScope {
public:
  AudioPreparationScope(const std::atomic_bool &cancelled, int outputRate = 0,
                        std::stop_token stop = {})
      : previous_(current_), root_(previous_ ? previous_->root_ : this),
        reservation_(AudioPreparationBudget::Shared()), cancelled_(cancelled),
        stop_(stop), outputRate_(outputRate) {
    current_ = this;
  }
  ~AudioPreparationScope() { current_ = previous_; }
  AudioPreparationScope(const AudioPreparationScope &) = delete;
  AudioPreparationScope &operator=(const AudioPreparationScope &) = delete;

  bool reserve(std::size_t samples, int channels, int sampleRate) {
    if (cancelled_ || stop_.stop_requested()) return false;
    if (root_ != this) return root_->reserve(samples, channels, sampleRate);
    if (channels <= 0 || sampleRate <= 0) return false;
    // Native PCM plus growth/compaction overlap, a converted output (or slice),
    // and decoder scratch. Saturate before conversion to size_t; oversized work
    // needs exclusive admission, not a giant counter or allocation.
    const long double ratio = std::max(1.0L,
        static_cast<long double>(outputRate_) / sampleRate);
    const long double bytes = samples * (2.0L + ratio) * sizeof(short) +
        static_cast<long double>(channels) * 4096 * sizeof(double);
    constexpr auto maximum = AudioPreparationBudget::kDefaultBytes;
    return reservation_.acquire(static_cast<std::size_t>(std::min<long double>(bytes, maximum)),
                                cancelled_, stop_);
  }

private:
  inline static thread_local AudioPreparationScope *current_ = nullptr;
  AudioPreparationScope *previous_;
  AudioPreparationScope *root_;
  AudioPreparationBudget::Reservation reservation_;
  const std::atomic_bool &cancelled_;
  std::stop_token stop_;
  int outputRate_;
};

} // namespace audio
