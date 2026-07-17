#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>

namespace gameplay {

enum class ManualKeysoundLane { None, Main, Compensation };

struct ManualKeysoundSelection {
  ManualKeysoundLane lane = ManualKeysoundLane::None;
  std::size_t index = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return lane != ManualKeysoundLane::None;
  }
};

template <typename Entry, typename TimingProjection>
[[nodiscard]] ManualKeysoundSelection selectManualKeysound(
    std::span<const Entry> mainNotes,
    std::span<const Entry> compensationNotes,
    std::int64_t inputTimeMicros, std::int64_t rangeStartMicros,
    std::int64_t rangeEndMicros, TimingProjection timing) {
  if (rangeStartMicros >= rangeEndMicros) {
    return {};
  }

  struct Candidate {
    bool valid = false;
    std::size_t index = 0;
    std::int64_t timingMicros = 0;
  };
  const auto before = [&](const Entry &entry, std::int64_t value) {
    return timing(entry) < value;
  };
  const auto eligibleBounds = [&](std::span<const Entry> notes) {
    const auto begin = std::lower_bound(notes.begin(), notes.end(),
                                        rangeStartMicros, before);
    const auto end =
        std::lower_bound(begin, notes.end(), rangeEndMicros, before);
    return std::pair{begin, end};
  };
  const auto futureCandidate = [&](std::span<const Entry> notes) {
    const auto [begin, end] = eligibleBounds(notes);
    const auto found = std::lower_bound(begin, end, inputTimeMicros, before);
    return found == end
               ? Candidate{}
               : Candidate{.valid = true,
                           .index = static_cast<std::size_t>(
                               std::distance(notes.begin(), found)),
                           .timingMicros = timing(*found)};
  };
  const auto lastCandidate = [&](std::span<const Entry> notes) {
    const auto [begin, end] = eligibleBounds(notes);
    if (begin == end) {
      return Candidate{};
    }
    const auto found = std::prev(end);
    return Candidate{.valid = true,
                     .index = static_cast<std::size_t>(
                         std::distance(notes.begin(), found)),
                     .timingMicros = timing(*found)};
  };

  const Candidate mainFuture = futureCandidate(mainNotes);
  const Candidate compensationFuture = futureCandidate(compensationNotes);
  if (mainFuture.valid || compensationFuture.valid) {
    if (mainFuture.valid &&
        (!compensationFuture.valid ||
         mainFuture.timingMicros <= compensationFuture.timingMicros)) {
      return {ManualKeysoundLane::Main, mainFuture.index};
    }
    return {ManualKeysoundLane::Compensation, compensationFuture.index};
  }

  const Candidate mainLast = lastCandidate(mainNotes);
  const Candidate compensationLast = lastCandidate(compensationNotes);
  if (mainLast.valid &&
      (!compensationLast.valid ||
       mainLast.timingMicros >= compensationLast.timingMicros)) {
    return {ManualKeysoundLane::Main, mainLast.index};
  }
  if (compensationLast.valid) {
    return {ManualKeysoundLane::Compensation, compensationLast.index};
  }
  return {};
}

} // namespace gameplay
