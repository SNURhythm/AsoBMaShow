#pragma once

#include <cassert>
#include <cstdint>

namespace gameplay_note_submission_order {

inline constexpr uint32_t kBackgroundDepth = 100U;
inline constexpr uint32_t kLaneBeamDepth = 180U;
inline constexpr uint32_t kFirstOrderedNoteDepth = 181U;
inline constexpr uint32_t kGhostDepth = 0x80000000U;
inline constexpr uint32_t kLaneCoverDepth = 0x90000000U;
inline constexpr uint32_t kIndicatorDepth = 0xA0000000U;
inline constexpr uint32_t kJudgementIndicatorDepth = 0xB0000000U;
inline constexpr uint32_t kGaugeDepth = 0xC0000000U;

struct LongNoteOrder {
  uint32_t bodyDepth = 0;
  uint32_t endpointDepth = 0;
};

class Allocator {
public:
  [[nodiscard]] uint32_t next() {
    assert(nextDepth < kGhostDepth && "note submission order exhausted");
    return nextDepth++;
  }

  [[nodiscard]] LongNoteOrder captureLongNote() {
    const uint32_t bodyDepth = next();
    return {.bodyDepth = bodyDepth, .endpointDepth = next()};
  }

  void reset() { nextDepth = kFirstOrderedNoteDepth; }

private:
  uint32_t nextDepth = kFirstOrderedNoteDepth;
};

} // namespace gameplay_note_submission_order
