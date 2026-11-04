#pragma once
#include <algorithm>
#include <span>

namespace rendering {
// GL readback starts at the bottom of the allocated texture, even when only
// the top portion of its final strip belongs to the exported image.
inline void normalizeBgraReadback(std::span<unsigned char> pixels,
                                  int width, int textureHeight, bool bottomUp) {
  if (!bottomUp) return;
  const auto stride=static_cast<std::size_t>(width)*4;
  for (int y=0;y<textureHeight/2;++y) {
    auto top=pixels.begin()+static_cast<std::size_t>(y)*stride;
    auto bottom=pixels.begin()+static_cast<std::size_t>(textureHeight-1-y)*stride;
    std::swap_ranges(top,top+stride,bottom);
  }
}
}
