#pragma once

#include "Judgement.h"
#include <cstdint>

namespace judgement_colors {
// Shared RGB accents for judgement text, counters, and default lane effects.
inline std::uint32_t rgb(Judgement judgement, bool light = false) {
  switch (judgement) {
  case PGreat: return light ? 0x00919C : 0x56D2CA;
  case Great: return light ? 0x61B022 : 0xB7F65C;
  case Good: return light ? 0xDE8F22 : 0xFFCC51;
  case Bad: return 0xFF8460;
  case Poor: return light ? 0xEE4C42 : 0xFF6858;
  case Kpoor: return 0xFF4E66;
  default: return light ? 0x081920 : 0xF8FDFF;
  }
}
} // namespace judgement_colors
