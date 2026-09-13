#include "scene/SettingsPreviewChart.h"
#include "bms_parser.hpp"

#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

namespace {
// The recipe is synchronous and uses ordinary allocations. Keep tracking
// allocation-free so injected failures exercise the real chart constructors.
std::array<void *, 512> liveAllocations{};
std::size_t liveCount = 0, allocationCount = 0;
std::size_t failAt = std::numeric_limits<std::size_t>::max();
bool tracking = false;

void *allocate(std::size_t size) {
  if (tracking && allocationCount++ == failAt) throw std::bad_alloc{};
  void *memory = std::malloc(size == 0 ? 1 : size);
  if (!memory) throw std::bad_alloc{};
  if (tracking) {
    for (auto &slot : liveAllocations) {
      if (!slot) {
        slot = memory;
        ++liveCount;
        return memory;
      }
    }
    std::abort();
  }
  return memory;
}

void release(void *memory) noexcept {
  if (!memory) return;
  for (auto &slot : liveAllocations) {
    if (slot == memory) {
      slot = nullptr;
      --liveCount;
      break;
    }
  }
  std::free(memory);
}
} // namespace

void *operator new(std::size_t size) { return allocate(size); }
void *operator new[](std::size_t size) { return allocate(size); }
void operator delete(void *memory) noexcept { release(memory); }
void operator delete[](void *memory) noexcept { release(memory); }
void operator delete(void *memory, std::size_t) noexcept { release(memory); }
void operator delete[](void *memory, std::size_t) noexcept { release(memory); }

void testRecipe() {
  const auto chart = settings_scene::makePreviewChart();
  assert(chart->Meta.Title == "Settings Preview");
  assert(chart->Meta.Bpm == 120 && chart->Meta.MinBpm == 120 && chart->Meta.MaxBpm == 120);
  assert(chart->Meta.KeyMode == 7 && !chart->Meta.IsDP && chart->Meta.Rank == 3);
  assert(chart->Meta.PlayLength == 8'000'000 && chart->Meta.TotalLength == 8'000'000);
  assert(chart->Measures.size() == 1);
  const auto &measure = *chart->Measures.front();
  assert(measure.Timing == 0 && measure.Scale == 4 && measure.Pos == 0);
  constexpr std::array<long long, 14> timings{
      500'000, 850'000, 1'200'000, 1'550'000, 1'900'000, 2'400'000, 3'900'000,
      4'300'000, 4'700'000, 5'200'000, 5'650'000, 6'100'000, 6'550'000, 7'000'000};
  constexpr std::array<int, 14> lanes{0, 2, 4, 6, 7, 3, 3, 1, 5, 0, 7, 2, 4, 6};
  assert(measure.TimeLines.size() == timings.size());
  for (std::size_t i = 0; i < timings.size(); ++i) {
    const auto &timeline = *measure.TimeLines[i];
    assert(timeline.Timing == timings[i] && timeline.Bpm == 120 && timeline.Scroll == 1);
    assert(timeline.BeatPosition == static_cast<double>(timings[i]) / 2'000'000.0);
    assert(timeline.IsFirstInMeasure == (i == 0));
    assert(timeline.Notes.size() == 16);
    for (int lane = 0; lane < 16; ++lane) {
      const auto *note = timeline.Notes[lane];
      assert((note != nullptr) == (lane == lanes[i]));
      if (note) assert(note->Lane == lane && note->Timeline == &timeline);
    }
  }
  const auto *head = dynamic_cast<bms_parser::LongNote *>(measure.TimeLines[5]->Notes[3]);
  const auto *tail = dynamic_cast<bms_parser::LongNote *>(measure.TimeLines[6]->Notes[3]);
  assert(head && tail && head->Tail == tail && tail->Head == head);
}

void testEveryConstructionAllocation() {
  tracking = true;
  { const auto chart = settings_scene::makePreviewChart(); }
  tracking = false;
  assert(liveCount == 0);
  const auto allocations = allocationCount;
  assert(allocations > 0);
  for (std::size_t index = 0; index < allocations; ++index) {
    allocationCount = 0;
    failAt = index;
    tracking = true;
    bool threw = false;
    try {
      const auto chart = settings_scene::makePreviewChart();
    } catch (const std::bad_alloc &) {
      threw = true;
    }
    tracking = false;
    if (!threw || liveCount != 0) {
      std::cerr << "Preview allocation " << index << ": threw=" << threw
                << ", live allocations=" << liveCount << '\n';
      std::abort();
    }
  }
  failAt = std::numeric_limits<std::size_t>::max();
  std::cout << "Preview construction passed " << allocations << " allocation failures\n";
}

int main() {
  testRecipe();
  testEveryConstructionAllocation();
  testRecipe();
}
