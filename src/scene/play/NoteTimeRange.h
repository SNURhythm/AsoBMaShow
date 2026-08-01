#pragma once

#include "../../bms_parser.hpp"

struct NoteTimeRange {
  long long startMicros = 0;
  long long endMicros = 0;

  [[nodiscard]] bool contains(long long timingMicros) const {
    return timingMicros >= startMicros && timingMicros < endMicros;
  }

  [[nodiscard]] bool contains(const bms_parser::Note *note) const {
    return note != nullptr && note->Timeline != nullptr &&
           contains(note->Timeline->Timing);
  }
};
