#include "MusicSelectFavorites.h"

MusicSelectFavoriteState musicSelectNextFavoriteState(
    int flags, MusicSelectFavoriteBits bits, int direction) {
  int state = 0;
  if ((flags & bits.invisible) != 0) {
    state = 2;
  } else if ((flags & bits.favorite) != 0) {
    state = 1;
  }
  state = (state + (direction >= 0 ? 1 : 2)) % 3;
  return static_cast<MusicSelectFavoriteState>(state);
}

int musicSelectApplyFavoriteState(int flags, MusicSelectFavoriteBits bits,
                                  MusicSelectFavoriteState state) {
  flags &= ~(bits.favorite | bits.invisible);
  switch (state) {
  case MusicSelectFavoriteState::None:
    break;
  case MusicSelectFavoriteState::Favorite:
    flags |= bits.favorite;
    break;
  case MusicSelectFavoriteState::Invisible:
    flags |= bits.invisible;
    break;
  }
  return flags;
}
