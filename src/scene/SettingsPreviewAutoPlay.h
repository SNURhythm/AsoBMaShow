#pragma once

#include "play/GameplayDefinition.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace settings_scene {

struct PreviewAutoPlayEvent {
  std::int64_t timeMicros = 0;
  int lane = -1;
  bool press = false;
  bool backSpin = false;
};

[[nodiscard]] inline std::vector<PreviewAutoPlayEvent> makePreviewAutoPlayEvents(
    const gameplay::GameplayDefinition &definition, bool randomTiming,
    std::mt19937 &random) {
  std::vector<PreviewAutoPlayEvent> events;
  std::unordered_map<int, std::int64_t> laneAvailableAt;
  std::bernoulli_distribution miss(0.1);
  std::bernoulli_distribution hitMine(0.35);
  std::uniform_int_distribution<std::int64_t> offset(-180'000, 180'000);
  for (const auto id : definition.chronologicalNotes()) {
    const auto &note = definition.note(id);
    if (!note.inActiveSlot ||
        (note.kind != gameplay::NoteKind::Normal &&
         note.kind != gameplay::NoteKind::LongHead &&
         note.kind != gameplay::NoteKind::Landmine)) continue;
    const bool mine = note.kind == gameplay::NoteKind::Landmine;
    if (mine && (!randomTiming || !hitMine(random))) continue;
    // Omitting both edges leaves the real simulation to judge the miss.
    if (!mine && randomTiming && miss(random)) continue;
    // Mines check whether the lane is held as their timeline passes.
    const auto timingOffset = mine ? -1 : randomTiming ? offset(random) : 0;
    const auto pressTime = std::max(note.timingMicros + timingOffset,
                                    laneAvailableAt[note.lane]);
    // Late taps release immediately after their ordinary tap interval, before
    // chart completion can stop accepting release inputs.
    const auto tapReleaseTime = std::max(pressTime,
        std::min(pressTime + 40'000, note.timingMicros + 40'000));
    const auto releaseTime = note.kind == gameplay::NoteKind::LongHead
        ? std::max(pressTime + 1, definition.note(note.pairId).timingMicros + timingOffset)
        : tapReleaseTime;
    events.push_back({pressTime, note.lane, true});
    const bool backSpin = note.kind == gameplay::NoteKind::LongHead && note.scratchLane &&
        (note.longNoteRule == gameplay::LongNoteRule::Charge ||
         note.longNoteRule == gameplay::LongNoteRule::HellCharge);
    events.push_back({releaseTime, note.lane, false, backSpin});
    laneAvailableAt[note.lane] = releaseTime + 1;
  }
  std::stable_sort(events.begin(), events.end(), [](const auto &left, const auto &right) {
    return left.timeMicros < right.timeMicros;
  });
  return events;
}

} // namespace settings_scene
