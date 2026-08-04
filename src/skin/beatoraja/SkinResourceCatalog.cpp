#include "SkinResourceCatalog.h"
#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include <limits>

namespace skin {
namespace {
SkinDiagnostic diagnostic(std::string code, std::string message) { return {.code=std::move(code), .message=std::move(message), .severity=DiagnosticSeverity::Error}; }
}
bool skinResourceDimensionsAllowed(int width, int height, std::size_t bytes) noexcept {
  if (width <= 0 || height <= 0 || width > SkinResourcePolicy::maximumDimension || height > SkinResourcePolicy::maximumDimension) return false;
  const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  return pixels <= std::numeric_limits<std::size_t>::max()/4U && bytes == static_cast<std::size_t>(pixels)*4U && bytes <= SkinResourcePolicy::maximumImageBytes && bytes <= UINT32_MAX;
}
bool skinResourceResolveRect(const SkinSourceRect &a, int iw, int ih, SkinSourceRect &r) noexcept {
  if (iw <= 0 || ih <= 0 || a.x < 0 || a.y < 0) return false;
  const int w = a.w == -1 ? iw : a.w, h = a.h == -1 ? ih : a.h;
  const int cols = a.gridColumns <= 0 ? 1 : a.gridColumns, rows = a.gridRows <= 0 ? 1 : a.gridRows;
  if (w <= 0 || h <= 0 || cols <= 0 || rows <= 0 || a.gridColumn < 0 || a.gridRow < 0 || a.gridColumn >= cols || a.gridRow >= rows || a.x > iw-w || a.y > ih-h) return false;
  const int cw = w / cols, ch = h / rows;
  if (cw <= 0 || ch <= 0) return false;
  r = a; r.x = a.x + cw*a.gridColumn; r.y = a.y + ch*a.gridRow; r.w = cw; r.h = ch; r.gridColumn=0; r.gridRow=0; r.gridColumns=1; r.gridRows=1; return true;
}
SkinResourceCatalog::SkinResourceCatalog(SkinRevisionLease &&revision, SkinTextureDevice &device) : revision_(std::move(revision)), device_(&device), owner_(std::this_thread::get_id()) {}
SkinResourceCatalog::~SkinResourceCatalog() { if (device_ && device_->ownsCurrentThread() && std::this_thread::get_id()==owner_) for (const auto &item: owned_) if (bgfx::isValid(item.handle)) device_->destroy(item.handle); }
SkinResourceUploadResult SkinResourceCatalog::upload(SkinResourceUploadPlan &&plan, SkinTextureDevice &device) {
  SkinResourceUploadResult result;
  if (!device.ownsCurrentThread()) { result.diagnostics.push_back(diagnostic("skin.resource.render_thread_violation", "resource upload requires the render owner thread")); return result; }
  auto catalog=std::unique_ptr<SkinResourceCatalog>(new SkinResourceCatalog(std::move(plan.revision), device));
  auto rollback=[&]{ catalog.reset(); };
  for (const auto &image : plan.images) { if (!skinResourceDimensionsAllowed(image.pixels.width,image.pixels.height,image.pixels.byteSize())) { result.diagnostics.push_back(diagnostic("skin.resource.image_dimensions","decoded image violates resource limits")); rollback(); return result; } const auto handle=device.create(image.pixels); if(!bgfx::isValid(handle)){result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed","texture creation failed")); rollback(); return result;} catalog->owned_.push_back({handle}); catalog->resources_.emplace(image.id, PreparedSkinResource{.id=image.id,.texture=handle,.width=image.pixels.width,.height=image.pixels.height,.regions=image.regions}); }
  for (const auto &atlas : plan.atlases) { if (!skinResourceDimensionsAllowed(atlas.pixels.width,atlas.pixels.height,atlas.pixels.byteSize())) { result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit","prepared atlas violates resource limits")); rollback(); return result; } const auto handle=device.create(atlas.pixels); if(!bgfx::isValid(handle)){result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed","texture creation failed")); rollback(); return result;} catalog->owned_.push_back({handle}); catalog->atlases_.emplace(atlas.id,PreparedSkinTextAtlas{.id=atlas.id,.key=atlas.key,.texture=handle,.width=atlas.pixels.width,.height=atlas.pixels.height,.glyphs=atlas.glyphs,.kerning=atlas.kerning,.ascent=atlas.ascent,.descent=atlas.descent,.lineHeight=atlas.lineHeight}); catalog->atlasKeys_.emplace(atlas.key,atlas.id); }
  result.catalog=std::move(catalog); return result;
}
const PreparedSkinResource *SkinResourceCatalog::find(SkinResourceId id) const noexcept { const auto it=resources_.find(id); return it==resources_.end()?nullptr:&it->second; }
const PreparedSkinTextAtlas *SkinResourceCatalog::findTextAtlas(SkinTextAtlasId id) const noexcept { const auto it=atlases_.find(id); return it==atlases_.end()?nullptr:&it->second; }
const PreparedSkinTextAtlas *SkinResourceCatalog::findTextAtlas(const SkinTextAtlasKey &key) const noexcept { const auto it=atlasKeys_.find(key); return it==atlasKeys_.end()?nullptr:findTextAtlas(it->second); }
} // namespace skin
#endif
