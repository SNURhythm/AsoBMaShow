#pragma once

#include <SDL3/SDL_scancode.h>

namespace input {
// SDL3 renumbered the consumer-key range. Ordinary USB keyboard and mobile
// scancodes are unchanged. Retain removed keys as negative, inert binding IDs
// so a saved action never silently attaches to an unrelated SDL3 key.
constexpr int migrateSdl2Scancode(int scancode) noexcept {
  switch (scancode) {
  case 258: return SDL_SCANCODE_MEDIA_NEXT_TRACK;
  case 259: return SDL_SCANCODE_MEDIA_PREVIOUS_TRACK;
  case 260: return SDL_SCANCODE_MEDIA_STOP;
  case 261: return SDL_SCANCODE_MEDIA_PLAY;
  case 262: return SDL_SCANCODE_MUTE;
  case 263: return SDL_SCANCODE_MEDIA_SELECT;
  case 268: return SDL_SCANCODE_AC_SEARCH;
  case 269: return SDL_SCANCODE_AC_HOME;
  case 270: return SDL_SCANCODE_AC_BACK;
  case 271: return SDL_SCANCODE_AC_FORWARD;
  case 272: return SDL_SCANCODE_AC_STOP;
  case 273: return SDL_SCANCODE_AC_REFRESH;
  case 274: return SDL_SCANCODE_AC_BOOKMARKS;
  case 281: return SDL_SCANCODE_MEDIA_EJECT;
  case 282: return SDL_SCANCODE_SLEEP;
  case 285: return SDL_SCANCODE_MEDIA_REWIND;
  case 286: return SDL_SCANCODE_MEDIA_FAST_FORWARD;
  default: return scancode >= 258 && scancode <= 286 ? -scancode : scancode;
  }
}
} // namespace input
