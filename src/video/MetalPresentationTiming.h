#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace video::presentationTiming {
enum class Event : unsigned {
  Callback, Deadline, Target, PreparationStart, PreparationEnd, RenderSubmit,
  GpuStart, GpuEnd, PresentRequest, PresentScheduled, Displayed, FrameEnd, Count
};
struct Sample {
  std::uint64_t token = 0, generation = 0;
  bool gameplay = false;
  std::array<double, static_cast<unsigned>(Event::Count)> times{};
};

template<std::size_t Capacity> class Capture {
  static_assert(Capacity > 0);
  struct Slot { std::atomic_flag busy = ATOMIC_FLAG_INIT; Sample value; };
  std::array<Slot, Capacity> slots_;
  std::atomic<std::uint64_t> next_{1}, dropped_{0};
public:
  std::uint64_t begin(double callback, double deadline, double target,
                      std::uint64_t generation) noexcept {
    const auto token = next_.fetch_add(1, std::memory_order_relaxed);
    auto &slot = slots_[token % Capacity];
    if (slot.busy.test_and_set(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
    slot.value = {};
    slot.value.token = token; slot.value.generation = generation;
    slot.value.times[static_cast<unsigned>(Event::Callback)] = callback;
    slot.value.times[static_cast<unsigned>(Event::Deadline)] = deadline;
    slot.value.times[static_cast<unsigned>(Event::Target)] = target;
    slot.busy.clear(std::memory_order_release);
    return token;
  }
  void record(std::uint64_t token, Event event, double seconds,
              bool gameplay = false) noexcept {
    if (!token || !std::isfinite(seconds) || seconds <= 0 || event == Event::Count) return;
    auto &slot = slots_[token % Capacity];
    if (slot.busy.test_and_set(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (slot.value.token == token) {
      slot.value.times[static_cast<unsigned>(event)] = seconds;
      if (event == Event::PreparationStart) slot.value.gameplay = gameplay;
    }
    slot.busy.clear(std::memory_order_release);
  }
  std::vector<Sample> snapshot() {
    std::vector<Sample> result;
    result.reserve(Capacity);
    for (auto &slot : slots_) {
      if (slot.busy.test_and_set(std::memory_order_acquire)) continue;
      const auto sample = slot.value;
      slot.busy.clear(std::memory_order_release);
      if (sample.token) result.push_back(sample);
    }
    std::sort(result.begin(), result.end(), [](const Sample &a, const Sample &b) { return a.token < b.token; });
    return result;
  }
  std::uint64_t droppedUpdates() const noexcept { return dropped_.load(std::memory_order_relaxed); }
};

// Late native callbacks may outlive the application/renderer. Never destroy
// storage underneath them; only tokens, never app or drawable pointers, escape.
inline Capture<8192> &capture() {
  static auto *storage = new Capture<8192>;
  return *storage;
}
inline std::string exportText(const std::vector<Sample> &samples, std::uint64_t dropped) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "\n\nMetal presentation timing v1; clock=CAMediaTime_seconds; capacity=8192; dropped_updates="
      << dropped << "; rows=" << samples.size()
      << "\nZeros are missing stages, not zero-duration work. Snapshot is best effort; busy slots may be omitted."
      << "\nScheduled-handler time is callback observation, not the exact drawable present call."
      << "\nframe,generation,gameplay,callback,deadline,target,preparation_start,preparation_end,render_submit,gpu_start,gpu_end,present_request,present_scheduled_handler,displayed,frame_end\n";
  out << std::fixed << std::setprecision(9);
  for (const auto &s : samples) {
    out << s.token << ',' << s.generation << ',' << s.gameplay;
    for (double time : s.times) out << ',' << time;
    out << '\n';
  }
  return out.str();
}
inline std::string exportText() {
  auto &storage = capture();
  const auto samples = storage.snapshot();
  return exportText(samples, storage.droppedUpdates());
}
} // namespace video::presentationTiming
