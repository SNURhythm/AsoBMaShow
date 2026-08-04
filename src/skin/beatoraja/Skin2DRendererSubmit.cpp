#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "Skin2DRenderer.h"
#include "../../rendering/SkinQuadBatchRenderer.h"

namespace skin {

bool Skin2DRenderer::submit(
    const SkinCommandBuffer &buffer, const SkinResourceCatalog &resources,
    RenderContext &context,
    rendering::SkinQuadBatchRenderer &renderer) const {
  renderer.begin(context, resources);
  if (!renderer.submit(buffer.commands)) {
    renderer.flush();
    return false;
  }
  renderer.flush();
  return true;
}

} // namespace skin

#endif
