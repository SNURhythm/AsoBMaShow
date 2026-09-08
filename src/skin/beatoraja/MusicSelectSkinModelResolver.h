#pragma once

#include "BeatorajaSkinModel.h"

#include <optional>

namespace skin {

struct MusicSelectSkinModelResolution {
  std::optional<SkinSongListObject> songList;
};

class MusicSelectSkinModelResolver final {
public:
  MusicSelectSkinModelResolution
  resolve(const BeatorajaSkinModel &model) const;
};

} // namespace skin
