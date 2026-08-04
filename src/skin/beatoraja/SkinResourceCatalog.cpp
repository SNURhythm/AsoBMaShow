#include "SkinResourceCatalog.h"
#include "../LuaGameplaySkinFeature.h"
#include "view/ImageFileDecoder.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <functional>
#include <set>
#include <type_traits>

namespace skin {
namespace {
SkinDiagnostic diagnostic(std::string code, std::string message) { return {.code=std::move(code), .message=std::move(message), .severity=DiagnosticSeverity::Error}; }
SkinDiagnostic warning(std::string code, std::string message) { return {.code=std::move(code), .message=std::move(message), .severity=DiagnosticSeverity::Warning}; }

struct ResourceUse {
  bool critical = false;
  std::vector<SkinSourceRect> regions;
};
struct CollectedResourceUses {
  std::map<SkinResourceId, ResourceUse> images;
  std::map<SkinResourceId, bool> fonts;
};

CollectedResourceUses collectResourceUses(const ValidatedBeatorajaSkinModel &model) {
  CollectedResourceUses result;
  std::map<SkinObjectId, const SkinObjectDefinition *> objects;
  std::set<SkinObjectId> disabled(model.disabledOptionalObjects.begin(), model.disabledOptionalObjects.end());
  for (const auto &object : model.model.objects) objects.emplace(object.id, &object);
  std::set<SkinObjectId> visited;
  const auto addSprite = [&](const SkinSpriteFrames &sprite, bool critical) {
    auto &use = result.images[sprite.resource];
    use.critical = use.critical || critical;
    use.regions.insert(use.regions.end(), sprite.frames.begin(), sprite.frames.end());
  };
  std::function<void(SkinObjectId, bool)> visit = [&](SkinObjectId id, bool critical) {
    if (disabled.contains(id) || !visited.insert(id).second) return;
    const auto found = objects.find(id); if (found == objects.end()) return;
    const auto &payload = found->second->payload;
    std::visit([&](const auto &object) {
      using T = std::decay_t<decltype(object)>;
      if constexpr (std::is_same_v<T, SkinImageObject>) { for (const auto &s: object.orderedStates) addSprite(s, critical); }
      else if constexpr (std::is_same_v<T, SkinNumberObject> || std::is_same_v<T, SkinFloatObject>) { addSprite(object.digits.positive, critical); if (object.digits.negative) addSprite(*object.digits.negative, critical); }
      else if constexpr (std::is_same_v<T, SkinTextObject>) result.fonts[object.font] = result.fonts[object.font] || critical;
      else if constexpr (std::is_same_v<T, SkinSliderObject>) addSprite(object.knob, critical);
      else if constexpr (std::is_same_v<T, SkinGraphObject>) addSprite(object.fill, critical);
      else if constexpr (std::is_same_v<T, SkinGaugeObject>) for (const auto &s: object.orderedNodes) addSprite(s, critical);
      else if constexpr (std::is_same_v<T, SkinNoteObject>) { for (const auto &lane: object.lanes) for (const auto &[kind, visual]: lane.visuals) { (void)kind; if (const auto *s=std::get_if<SkinSpriteFrames>(&visual)) addSprite(*s, critical); } for (const auto &line: object.lines) if (line.sprite) addSprite(*line.sprite, critical); }
      else if constexpr (std::is_same_v<T, SkinCoverObject>) addSprite(object.sprite, critical);
      else if constexpr (std::is_same_v<T, SkinJudgeObject>) for (const auto &grade: object.grades) { if (grade.image) visit(grade.image->object, critical); if (grade.detailNumber) visit(grade.detailNumber->object, critical); }
    }, payload);
  };
  // Critical roots run first so a nested optional object inherits criticality.
  for (const auto &object : model.model.objects) if (object.critical) visit(object.id, true);
  for (const auto &object : model.model.objects) if (!object.critical) visit(object.id, false);
  return result;
}

SkinDiagnostic useDiagnostic(std::string criticalCode, std::string optionalCode,
                            std::string message, bool critical) {
  return {.code = critical ? std::move(criticalCode) : std::move(optionalCode),
          .message = std::move(message),
          .severity = critical ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning};
}

bool resolveRegions(const ResourceUse &use, int width, int height,
                    std::vector<SkinSourceRect> &output,
                    std::vector<SkinDiagnostic> &diagnostics) {
  if (use.regions.size() > SkinResourcePolicy::maximumRegions ||
      output.size() > SkinResourcePolicy::maximumRegions - use.regions.size()) {
    diagnostics.push_back(useDiagnostic("skin.resource.session_limit", "skin.resource.session_limit", "sprite region count exceeds the resource policy", use.critical));
    return false;
  }
  for (const SkinSourceRect &authored : use.regions) {
    SkinSourceRect resolved;
    if (!skinResourceResolveRect(authored, width, height, resolved)) {
      diagnostics.push_back(useDiagnostic("skin.resource.sprite_bounds", "skin.resource.sprite_bounds", "sprite crop is outside its decoded image", use.critical));
      return false;
    }
    output.push_back(resolved);
  }
  return true;
}

bool runtimeStringsWithinPolicy(std::span<const std::string> strings,
                                std::vector<SkinDiagnostic> &diagnostics) {
  if (strings.size() > SkinResourcePolicy::maximumRuntimeStrings) {
    diagnostics.push_back(diagnostic("skin.resource.session_limit",
                                     "runtime string count exceeds the resource policy"));
    return false;
  }
  std::size_t bytes = 0;
  for (const std::string &value : strings) {
    if (value.size() > SkinResourcePolicy::maximumRuntimeStringBytes - bytes) {
      diagnostics.push_back(diagnostic("skin.resource.session_limit",
                                       "runtime string bytes exceed the resource policy"));
      return false;
    }
    bytes += value.size();
  }
  return true;
}

SkinDiagnostic fileDiagnostic(const SkinImageResource &resource,
                              const SkinFileFailure *failure, bool critical) {
  if (failure && failure->message == "resource path is ambiguous") {
    return useDiagnostic("skin.resource.path_ambiguous", "skin.resource.path_ambiguous",
                         "resource path resolves to multiple package files: " + resource.authoredName, critical);
  }
  const bool missing = failure && failure->code == SkinFileError::Missing;
  return useDiagnostic(missing ? "skin.resource.missing_critical" : "skin.resource.path_invalid",
                       missing ? "skin.resource.missing_optional" : "skin.resource.path_invalid",
                       "image resource is unavailable: " + resource.authoredName, critical);
}

bool checkInput(const SkinEntryId &entry, SkinRevisionReadView revision,
                const LuaSkinFileSystem &files,
                std::vector<SkinDiagnostic> &diagnostics) {
  const SkinRevision &expected = revision.revision();
  const SkinRevision &actual = files.revision();
  if (entry != files.entry() || expected.package != actual.package ||
      expected.lowercaseSha256 != actual.lowercaseSha256 ||
      expected.fileCount != actual.fileCount || expected.totalBytes != actual.totalBytes ||
      revision.root().lexically_normal() != files.revisionRoot().lexically_normal()) {
    diagnostics.push_back(diagnostic("skin.resource.path_invalid",
                                     "resource preparation entry does not match its filesystem"));
    return false;
  }
  return true;
}
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
SkinResourceCatalog::SkinResourceCatalog(
    SkinRevisionLease &&revision, std::shared_ptr<SkinTextureDevice> device)
    : revision_(std::move(revision)), device_(std::move(device)),
      owner_(std::this_thread::get_id()) {}
SkinResourceCatalog::~SkinResourceCatalog() {
  if (!device_) return;
  if (!device_->ownsCurrentThread() || std::this_thread::get_id() != owner_) {
    std::terminate();
  }
  for (const auto &item : owned_)
    if (bgfx::isValid(item.handle)) device_->destroy(item.handle);
}
SkinResourceUploadResult SkinResourceCatalog::upload(
    SkinResourceUploadPlan &&plan, std::shared_ptr<SkinTextureDevice> device) {
  SkinResourceUploadResult result;
  if (!device || !device->ownsCurrentThread()) { result.diagnostics.push_back(diagnostic("skin.resource.render_thread_violation", "resource upload requires the render owner thread")); return result; }
  if (plan.images.size() > SkinResourcePolicy::maximumResources ||
      plan.atlases.size() > SkinResourcePolicy::maximumAtlases ||
      plan.decodedBytes > SkinResourcePolicy::maximumSessionDecodedBytes) {
    result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan exceeds fixed limits"));
    return result;
  }
  std::set<SkinResourceId> imageIds;
  std::set<SkinTextAtlasId> atlasIds;
  std::set<SkinTextAtlasKey> atlasKeys;
  std::size_t regions = 0;
  std::size_t atlasBytes = 0;
  std::size_t decodedBytes = 0;
  for (const auto &image : plan.images) {
    if (!imageIds.insert(image.id).second ||
        image.aliases.size() > SkinResourcePolicy::maximumResources - imageIds.size()) {
      result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has duplicate image IDs")); return result;
    }
    for (SkinResourceId alias : image.aliases) if (!imageIds.insert(alias).second) { result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has duplicate image aliases")); return result; }
    if (!skinResourceDimensionsAllowed(image.pixels.width, image.pixels.height, image.pixels.byteSize()) ||
        image.pixels.byteSize() > SkinResourcePolicy::maximumSessionDecodedBytes - decodedBytes) {
      result.diagnostics.push_back(diagnostic("skin.resource.image_dimensions", "resource upload plan has invalid image dimensions or bytes")); return result;
    }
    decodedBytes += image.pixels.byteSize();
    if (image.regions.size() > SkinResourcePolicy::maximumRegions - regions) { result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource region count exceeds fixed limit")); return result; }
    regions += image.regions.size();
    for (SkinResourceId alias : image.aliases) {
      const auto aliasRegions = image.aliasRegions.find(alias);
      const std::size_t count = aliasRegions == image.aliasRegions.end()
          ? image.regions.size() : aliasRegions->second.size();
      if (count > SkinResourcePolicy::maximumRegions - regions) {
        result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource region count exceeds fixed limit")); return result;
      }
      regions += count;
    }
    for (const auto &[alias, aliasRegions] : image.aliasRegions) {
      if (std::find(image.aliases.begin(), image.aliases.end(), alias) == image.aliases.end() ||
          aliasRegions.size() > SkinResourcePolicy::maximumRegions) {
        result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has invalid alias regions")); return result;
      }
    }
  }
  for (const auto &atlas : plan.atlases) {
    const auto finite = [](double value) { return std::isfinite(value) && std::abs(value) <= 4096.0; };
    if (!finite(atlas.key.outlineWidth) || !finite(atlas.key.shadowOffsetX) ||
        !finite(atlas.key.shadowOffsetY) || !finite(atlas.key.shadowSmoothness) ||
        !atlasIds.insert(atlas.id).second || !atlasKeys.insert(atlas.key).second ||
        !skinResourceDimensionsAllowed(atlas.pixels.width, atlas.pixels.height, atlas.pixels.byteSize()) ||
        atlas.pixels.byteSize() > SkinResourcePolicy::maximumAtlasBytes ||
        atlas.pixels.byteSize() > SkinResourcePolicy::maximumAtlasSessionBytes - atlasBytes ||
        atlas.glyphs.size() > SkinResourcePolicy::maximumGlyphs || atlas.kerning.size() > SkinResourcePolicy::maximumKerningPairs) {
      result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit", "resource upload plan has invalid or duplicate atlas data")); return result;
    }
    atlasBytes += atlas.pixels.byteSize();
    if (atlas.pixels.byteSize() > SkinResourcePolicy::maximumSessionDecodedBytes - decodedBytes) {
      result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan decoded bytes exceed fixed limits")); return result;
    }
    decodedBytes += atlas.pixels.byteSize();
  }
  if (decodedBytes != plan.decodedBytes) {
    result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan decoded byte total is inconsistent")); return result;
  }
  try {
  auto catalog=std::unique_ptr<SkinResourceCatalog>(new SkinResourceCatalog(std::move(plan.revision), std::move(device)));
  catalog->owned_.reserve(plan.images.size() + plan.atlases.size());
  auto rollback=[&]{ catalog.reset(); };
  struct PendingHandle { SkinTextureDevice &device; bgfx::TextureHandle handle = BGFX_INVALID_HANDLE; ~PendingHandle() { if (bgfx::isValid(handle)) device.destroy(handle); } void release() noexcept { handle = BGFX_INVALID_HANDLE; } };
  for (const auto &image : plan.images) { const auto handle=catalog->device_->create(image.pixels); if(!bgfx::isValid(handle)){result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed","texture creation failed")); rollback(); return result;} PendingHandle pending{*catalog->device_, handle}; const PreparedSkinResource prepared{.id=image.id,.texture=handle,.width=image.pixels.width,.height=image.pixels.height,.regions=image.regions}; catalog->resources_.emplace(image.id, prepared); for (SkinResourceId alias : image.aliases) { const auto found=image.aliasRegions.find(alias); const auto &regions=found==image.aliasRegions.end()?image.regions:found->second; catalog->resources_.emplace(alias, PreparedSkinResource{.id=alias,.texture=handle,.width=image.pixels.width,.height=image.pixels.height,.regions=regions}); } catalog->owned_.push_back({handle}); pending.release(); }
  for (const auto &atlas : plan.atlases) { const auto handle=catalog->device_->create(atlas.pixels); if(!bgfx::isValid(handle)){result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed","texture creation failed")); rollback(); return result;} PendingHandle pending{*catalog->device_, handle}; catalog->atlases_.emplace(atlas.id,PreparedSkinTextAtlas{.id=atlas.id,.key=atlas.key,.texture=handle,.width=atlas.pixels.width,.height=atlas.pixels.height,.glyphs=atlas.glyphs,.kerning=atlas.kerning,.ascent=atlas.ascent,.descent=atlas.descent,.lineHeight=atlas.lineHeight}); catalog->atlasKeys_.emplace(atlas.key,atlas.id); catalog->owned_.push_back({handle}); pending.release(); }
  result.catalog=std::move(catalog); return result;
  } catch (...) {
    result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload allocation failed"));
    return result;
  }
}
const PreparedSkinResource *SkinResourceCatalog::find(SkinResourceId id) const noexcept { const auto it=resources_.find(id); return it==resources_.end()?nullptr:&it->second; }
const PreparedSkinTextAtlas *SkinResourceCatalog::findTextAtlas(SkinTextAtlasId id) const noexcept { const auto it=atlases_.find(id); return it==atlases_.end()?nullptr:&it->second; }
const PreparedSkinTextAtlas *SkinResourceCatalog::findTextAtlas(const SkinTextAtlasKey &key) const noexcept { const auto it=atlasKeys_.find(key); return it==atlasKeys_.end()?nullptr:findTextAtlas(it->second); }

SkinResourcePreparationService::SkinResourcePreparationService()
    : SkinResourcePreparationService(
          [](std::span<const std::byte> encoded, std::stop_token stop)
              -> std::optional<image_decode::DecodedImageData> {
            if (stop.stop_requested()) return std::nullopt;
            return image_decode::decodeImageMemory(
                encoded, SkinResourcePolicy::maximumDimension,
                SkinResourcePolicy::maximumImageBytes);
          },
          SkinResourcePolicy::workerCount) {}

SkinResourcePreparationService::SkinResourcePreparationService(
    Decoder decoder, std::size_t workerCount)
    : cache_(SkinResourcePolicy::cacheByteBudget),
      decoder_(std::move(decoder)),
      coordinator_([this](const image_decode::ImageDecodeRequest &request,
                          std::stop_token stop)
                       -> std::optional<image_decode::DecodedImageData> {
        if (stop.stop_requested() || !request.encoded || !decoder_) return std::nullopt;
        return decoder_(std::span<const std::byte>(request.encoded->data(),
                                                   request.encoded->size()),
                        stop);
      }, workerCount) {}

SkinResourcePreparationService::~SkinResourcePreparationService() { shutdown(); }

bool SkinResourcePreparationService::beginCall() {
  std::lock_guard lock(serviceMutex_);
  if (state_ != State::Running) return false;
  ++activeCalls_;
  return true;
}
void SkinResourcePreparationService::endCall() noexcept {
  std::lock_guard lock(serviceMutex_);
  if (activeCalls_ != 0) --activeCalls_;
  serviceCv_.notify_all();
}
void SkinResourcePreparationService::shutdown() noexcept {
  bool leader = false;
  {
    std::unique_lock lock(serviceMutex_);
    if (state_ == State::Running) {
      state_ = State::Stopping;
      leader = true;
    } else {
      serviceCv_.wait(lock, [this] { return shutdownComplete_; });
      return;
    }
  }
  if (leader) coordinator_.shutdown();
  std::unique_lock lock(serviceMutex_);
  serviceCv_.wait(lock, [this] { return activeCalls_ == 0; });
  state_ = State::Stopped;
  shutdownComplete_ = true;
  lock.unlock();
  serviceCv_.notify_all();
}

SkinResourceValidationResult SkinResourcePreparationService::validateResources(
    SkinResourceValidationInputs input) {
  SkinResourceValidationResult result;
  if (!beginCall()) { result.diagnostics.push_back(diagnostic("skin.resource.service_stopped", "resource preparation service is stopped")); return result; }
  struct End { SkinResourcePreparationService *service; ~End(){ service->endCall(); } } end{this};
  if (!checkInput(input.entry, input.revision, input.fileSystem, result.diagnostics) ||
      !runtimeStringsWithinPolicy(input.requiredRuntimeStrings, result.diagnostics)) return result;
  const CollectedResourceUses uses = collectResourceUses(input.model);
  std::set<std::string> resolved;
  std::map<std::string, image_decode::DecodedImageData, std::less<>> decodedByPath;
  std::size_t sessionBytes = 0;
  for (const SkinResourceDefinition &definition : input.model.model.resources) {
    if (input.stop.stop_requested()) { result.cancelled = true; return result; }
    const auto *resource = std::get_if<SkinImageResource>(&definition);
    if (!resource) continue; // Font rasterization is intentionally a later slice.
    const auto use = uses.images.find(resource->id);
    if (use == uses.images.end()) continue;
    const auto candidate = input.fileSystem.resolveResourceCandidates(resource->virtualPath, resource->virtualPath);
    if (!candidate.normalizedVirtualPath) {
      result.diagnostics.push_back(fileDiagnostic(*resource, candidate.failure ? &*candidate.failure : nullptr, use->second.critical));
      continue;
    }
    auto decoded = decodedByPath.find(*candidate.normalizedVirtualPath);
    if (decoded == decodedByPath.end()) {
      const auto read = input.fileSystem.readResolvedResource(*candidate.normalizedVirtualPath, SkinResourcePolicy::maximumEncodedBytes);
      if (read.failure) { result.diagnostics.push_back(fileDiagnostic(*resource, &*read.failure, use->second.critical)); continue; }
      if (resolved.insert(*candidate.normalizedVirtualPath).second) {
        if (read.bytes.size() > SkinResourcePolicy::maximumSessionEncodedBytes - sessionBytes) { result.diagnostics.push_back(useDiagnostic("skin.resource.session_limit", "skin.resource.session_limit", "encoded resource bytes exceed the session policy", use->second.critical)); continue; }
        sessionBytes += read.bytes.size();
      }
      const auto image = image_decode::decodeImageMemory(read.bytes, SkinResourcePolicy::maximumDimension, SkinResourcePolicy::maximumImageBytes);
      if (!image || !skinResourceDimensionsAllowed(image->width, image->height, image->byteSize())) { result.diagnostics.push_back(useDiagnostic("skin.resource.image_decode_failed", "skin.resource.image_decode_failed", "image decode failed resource validation", use->second.critical)); continue; }
      decoded = decodedByPath.emplace(*candidate.normalizedVirtualPath, *image).first;
    }
    std::vector<SkinSourceRect> ignored;
    (void)resolveRegions(use->second, decoded->second.width, decoded->second.height, ignored, result.diagnostics);
  }
  result.valid = std::ranges::none_of(result.diagnostics, [](const SkinDiagnostic &d) { return d.severity == DiagnosticSeverity::Error; });
  return result;
}

SkinResourcePlanResult SkinResourcePreparationService::decodeAndPlan(
    SkinResourcePreparationInputs input) {
  SkinResourcePlanResult result;
  if (!beginCall()) { result.diagnostics.push_back(diagnostic("skin.resource.service_stopped", "resource preparation service is stopped")); return result; }
  struct End { SkinResourcePreparationService *service; ~End(){ service->endCall(); } } end{this};
  if (!checkInput(input.entry, input.revision.readView(), input.fileSystem, result.diagnostics) ||
      !runtimeStringsWithinPolicy(input.requiredRuntimeStrings, result.diagnostics)) return result;
  const CollectedResourceUses uses = collectResourceUses(input.model);
  SkinResourceUploadPlan plan{.revision = std::move(input.revision)};
  std::map<std::string, std::size_t, std::less<>> unique;
  std::size_t encodedBytes = 0;
  for (const SkinResourceDefinition &definition : input.model.model.resources) {
    if (input.stop.stop_requested()) { result.cancelled = true; return result; }
    const auto *resource = std::get_if<SkinImageResource>(&definition);
    if (!resource) continue;
    const auto use = uses.images.find(resource->id);
    if (use == uses.images.end()) continue;
    const auto candidate = input.fileSystem.resolveResourceCandidates(resource->virtualPath, resource->virtualPath);
    if (!candidate.normalizedVirtualPath) { result.diagnostics.push_back(fileDiagnostic(*resource, candidate.failure ? &*candidate.failure : nullptr, use->second.critical)); continue; }
    if (const auto found = unique.find(*candidate.normalizedVirtualPath); found != unique.end()) { auto &image=plan.images[found->second]; std::vector<SkinSourceRect> regions; if (resolveRegions(use->second,image.pixels.width,image.pixels.height,regions,result.diagnostics)) { image.aliases.push_back(resource->id); image.aliasRegions.emplace(resource->id,std::move(regions)); } continue; }
    if (unique.size() == SkinResourcePolicy::maximumResources) { result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource count exceeds the session policy")); break; }
    const auto read = input.fileSystem.readResolvedResource(*candidate.normalizedVirtualPath, SkinResourcePolicy::maximumEncodedBytes);
    if (read.failure) { result.diagnostics.push_back(fileDiagnostic(*resource, &*read.failure, use->second.critical)); continue; }
    if (read.bytes.size() > SkinResourcePolicy::maximumSessionEncodedBytes - encodedBytes) { result.diagnostics.push_back(useDiagnostic("skin.resource.encoded_limit", "skin.resource.encoded_limit", "encoded resource bytes exceed the session policy", use->second.critical)); continue; }
    encodedBytes += read.bytes.size();
    const std::string key = plan.revision.revision().lowercaseSha256 + ":" + *candidate.normalizedVirtualPath;
    std::optional<image_decode::DecodedImageData> decoded;
    { std::lock_guard lock(serviceMutex_); decoded = cache_.get(key); }
    if (!decoded) {
      auto owned = std::make_shared<const std::vector<std::byte>>(std::move(read.bytes));
      const auto ticket = coordinator_.request({.key=key, .path={}, .encoded=std::move(owned)});
      const auto waited = coordinator_.waitTake(ticket, input.stop);
      if (waited.state == image_decode::ImageDecodeWaitState::Cancelled || waited.state == image_decode::ImageDecodeWaitState::Stopped || input.stop.stop_requested()) { result.cancelled = true; return result; }
      if (waited.state != image_decode::ImageDecodeWaitState::Ready || !waited.image) { result.diagnostics.push_back(useDiagnostic("skin.resource.image_decode_failed", "skin.resource.image_decode_failed", "image decode failed during planning", use->second.critical)); continue; }
      decoded = waited.image;
      { std::lock_guard lock(serviceMutex_); cache_.put(key, *decoded); }
    }
    if (!skinResourceDimensionsAllowed(decoded->width, decoded->height, decoded->byteSize()) || decoded->byteSize() > SkinResourcePolicy::maximumSessionDecodedBytes - plan.decodedBytes) { result.diagnostics.push_back(useDiagnostic("skin.resource.session_limit", "skin.resource.session_limit", "decoded resource bytes exceed the session policy", use->second.critical)); continue; }
    std::vector<SkinSourceRect> regions;
    if (!resolveRegions(use->second, decoded->width, decoded->height, regions, result.diagnostics)) continue;
    plan.decodedBytes += decoded->byteSize();
    unique.emplace(*candidate.normalizedVirtualPath, plan.images.size());
    plan.images.push_back({.id=resource->id, .pixels=*decoded, .regions=std::move(regions)});
  }
  if (std::ranges::any_of(result.diagnostics, [](const SkinDiagnostic &d) { return d.severity == DiagnosticSeverity::Error; })) return result;
  result.plan = std::move(plan);
  return result;
}
} // namespace skin
#endif
