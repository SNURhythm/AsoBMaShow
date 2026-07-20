#include "GameplayNoteJudgeRole.h"

#include "../../CoursePlaySession.h"
#include "../../bms_parser.hpp"

namespace gameplay {

NoteJudgeRole judgeRoleFor(const NoteDefinition &note) noexcept {
  switch (note.kind) {
  case NoteKind::LongHead:
    return note.scratchLane ? NoteJudgeRole::LongScratchHead
                            : NoteJudgeRole::LongNoteHead;
  case NoteKind::LongTail:
    return note.scratchLane ? NoteJudgeRole::LongScratchTail
                            : NoteJudgeRole::LongNoteTail;
  case NoteKind::Normal:
  case NoteKind::Landmine:
    return note.scratchLane ? NoteJudgeRole::Scratch
                            : NoteJudgeRole::Normal;
  }
  return NoteJudgeRole::Normal;
}

NoteJudgeRole judgeRoleFor(const bms_parser::Note *note,
                           const bms_parser::ChartMeta &chartMeta,
                           int longNoteModeOverride) noexcept {
  (void)longNoteModeOverride;
  if (note == nullptr) {
    return NoteJudgeRole::Normal;
  }
  const bool scratch = chartLaneIsScratch(chartMeta, note->Lane);
  const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
  if (longNote == nullptr) {
    return scratch ? NoteJudgeRole::Scratch : NoteJudgeRole::Normal;
  }
  if (longNote->IsTail()) {
    return scratch ? NoteJudgeRole::LongScratchTail
                   : NoteJudgeRole::LongNoteTail;
  }
  return scratch ? NoteJudgeRole::LongScratchHead
                 : NoteJudgeRole::LongNoteHead;
}

} // namespace gameplay
