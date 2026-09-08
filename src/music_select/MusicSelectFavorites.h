#pragma once

struct MusicSelectFavoriteBits {
  int favorite = 0;
  int invisible = 0;
};

enum class MusicSelectFavoriteState { None, Favorite, Invisible };

[[nodiscard]] MusicSelectFavoriteState musicSelectNextFavoriteState(
    int flags, MusicSelectFavoriteBits bits, int direction);

[[nodiscard]] int musicSelectApplyFavoriteState(
    int flags, MusicSelectFavoriteBits bits, MusicSelectFavoriteState state);
