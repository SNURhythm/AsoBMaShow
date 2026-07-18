#pragma once

#include "GameplayDefinition.h"
#include "GameplayJudgeRules.h"

namespace bms_parser {
struct ChartMeta;
class Note;
} // namespace bms_parser

namespace gameplay {

[[nodiscard]] NoteJudgeRole
judgeRoleFor(const NoteDefinition &note) noexcept;

[[nodiscard]] NoteJudgeRole
judgeRoleFor(const bms_parser::Note *note,
             const bms_parser::ChartMeta &chartMeta,
             int longNoteModeOverride) noexcept;

} // namespace gameplay
