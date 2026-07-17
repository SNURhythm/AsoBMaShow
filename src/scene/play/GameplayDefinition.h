#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace bms_parser {
class Chart;
}

namespace gameplay {

using NoteId = std::uint32_t;
inline constexpr NoteId kInvalidNoteId =
    std::numeric_limits<NoteId>::max();

enum class NoteKind { Normal, LongHead, LongTail, Landmine };
enum class LongNoteRule { None, Classic, Charge, HellCharge };

struct NoteDefinition {
  NoteId id = kInvalidNoteId;
  int lane = -1;
  std::int64_t timingMicros = 0;
  int wav = 0;
  NoteKind kind = NoteKind::Normal;
  LongNoteRule longNoteRule = LongNoteRule::None;
  NoteId pairId = kInvalidNoteId;
  bool scratchLane = false;
  float mineDamage = 0.0F;
};

struct LaneDefinition {
  int lane = -1;
  std::vector<NoteId> noteIds;
};

struct GameplayChartMetadata {
  int totalNotes = 0;
  int keyMode = 7;
  double gaugeTotal = 100.0;
  std::int64_t finalNoteTimeMicros = 0;
  std::int64_t finalTimelineTimeMicros = 0;
};

class GameplayDefinition {
public:
  [[nodiscard]] std::size_t noteCount() const noexcept;
  [[nodiscard]] const NoteDefinition &note(NoteId id) const;
  [[nodiscard]] std::span<const NoteId> laneNotes(int lane) const noexcept;
  [[nodiscard]] std::span<const LaneDefinition> lanes() const noexcept;
  [[nodiscard]] GameplayChartMetadata metadata() const noexcept;
  [[nodiscard]] std::span<const NoteId>
  chronologicalNotes() const noexcept;
  [[nodiscard]] std::span<const NoteId> hellChargeHeads() const noexcept;

private:
  friend GameplayDefinition buildGameplayDefinition(
      const bms_parser::Chart &, int);
  GameplayChartMetadata metadata_;
  std::vector<NoteDefinition> notes_;
  std::vector<LaneDefinition> lanes_;
  std::vector<NoteId> chronologicalNoteIds_;
  std::vector<NoteId> hellChargeHeadIds_;
};

GameplayDefinition buildGameplayDefinition(const bms_parser::Chart &chart,
                                           int longNoteModeOverride);

} // namespace gameplay
