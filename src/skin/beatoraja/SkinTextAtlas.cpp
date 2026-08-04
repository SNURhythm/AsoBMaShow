#include "SkinTextAtlas.h"
#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include <cmath>
#include <sstream>

namespace skin {
namespace {
bool quantize(double &value) noexcept {
  if (!std::isfinite(value) || std::abs(value) > 4096.0) return false;
  value = std::round(value * 64.0) / 64.0;
  return true;
}
}
bool canonicalizeSkinTextAtlasKey(SkinTextAtlasKey &key) noexcept {
  return key.font != 0 && key.pointSize > 0 && key.pointSize <= 512 &&
         quantize(key.outlineWidth) && quantize(key.shadowOffsetX) &&
         quantize(key.shadowOffsetY) && quantize(key.shadowSmoothness);
}
std::string stableFallbackChainDigest(SkinResourceId primary, int primaryType,
                                      const std::vector<SkinFontFallbackResource> &fallbacks) {
  std::ostringstream result;
  result << primary << ':' << primaryType;
  for (const auto &fallback : fallbacks)
    result << '|' << fallback.virtualPath.size() << ':' << fallback.virtualPath << ':' << fallback.type;
  return result.str();
}
} // namespace skin
#endif
