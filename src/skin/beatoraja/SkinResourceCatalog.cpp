#include "SkinResourceCatalog.h"
#include "../LuaGameplaySkinFeature.h"
#include "view/ImageFileDecoder.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include <limits>

namespace skin {
namespace {
SkinDiagnostic diagnostic(std::string code, std::string message) { return {.code=std::move(code), .message=std::move(message), .severity=DiagnosticSeverity::Error}; }
SkinDiagnostic warning(std::string code, std::string message) { return {.code=std::move(code), .message=std::move(message), .severity=DiagnosticSeverity::Warning}; }

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
                              const SkinFileFailure *failure) {
  if (failure && failure->message == "resource path is ambiguous") {
    return diagnostic("skin.resource.path_ambiguous",
                      "resource path resolves to multiple package files: " +
                          resource.authoredName);
  }
  const bool missing = failure && failure->code == SkinFileError::Missing;
  return diagnostic(missing ? "skin.resource.missing_critical"
                            : "skin.resource.path_invalid",
                    "required image resource is unavailable: " + resource.authoredName);
}

bool checkInput(const SkinEntryId &entry, const LuaSkinFileSystem &files,
                std::vector<SkinDiagnostic> &diagnostics) {
  if (entry != files.entry()) {
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
SkinResourceCatalog::SkinResourceCatalog(SkinRevisionLease &&revision, SkinTextureDevice &device) : revision_(std::move(revision)), device_(&device), owner_(std::this_thread::get_id()) {}
SkinResourceCatalog::~SkinResourceCatalog() { if (device_ && device_->ownsCurrentThread() && std::this_thread::get_id()==owner_) for (const auto &item: owned_) if (bgfx::isValid(item.handle)) device_->destroy(item.handle); }
SkinResourceUploadResult SkinResourceCatalog::upload(SkinResourceUploadPlan &&plan, SkinTextureDevice &device) {
  SkinResourceUploadResult result;
  if (!device.ownsCurrentThread()) { result.diagnostics.push_back(diagnostic("skin.resource.render_thread_violation", "resource upload requires the render owner thread")); return result; }
  auto catalog=std::unique_ptr<SkinResourceCatalog>(new SkinResourceCatalog(std::move(plan.revision), device));
  auto rollback=[&]{ catalog.reset(); };
  for (const auto &image : plan.images) { if (!skinResourceDimensionsAllowed(image.pixels.width,image.pixels.height,image.pixels.byteSize())) { result.diagnostics.push_back(diagnostic("skin.resource.image_dimensions","decoded image violates resource limits")); rollback(); return result; } const auto handle=device.create(image.pixels); if(!bgfx::isValid(handle)){result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed","texture creation failed")); rollback(); return result;} catalog->owned_.push_back({handle}); const PreparedSkinResource prepared{.id=image.id,.texture=handle,.width=image.pixels.width,.height=image.pixels.height,.regions=image.regions}; catalog->resources_.emplace(image.id, prepared); for (SkinResourceId alias : image.aliases) catalog->resources_.emplace(alias, PreparedSkinResource{.id=alias,.texture=handle,.width=image.pixels.width,.height=image.pixels.height,.regions=image.regions}); }
  for (const auto &atlas : plan.atlases) { if (!skinResourceDimensionsAllowed(atlas.pixels.width,atlas.pixels.height,atlas.pixels.byteSize())) { result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit","prepared atlas violates resource limits")); rollback(); return result; } const auto handle=device.create(atlas.pixels); if(!bgfx::isValid(handle)){result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed","texture creation failed")); rollback(); return result;} catalog->owned_.push_back({handle}); catalog->atlases_.emplace(atlas.id,PreparedSkinTextAtlas{.id=atlas.id,.key=atlas.key,.texture=handle,.width=atlas.pixels.width,.height=atlas.pixels.height,.glyphs=atlas.glyphs,.kerning=atlas.kerning,.ascent=atlas.ascent,.descent=atlas.descent,.lineHeight=atlas.lineHeight}); catalog->atlasKeys_.emplace(atlas.key,atlas.id); }
  result.catalog=std::move(catalog); return result;
}
const PreparedSkinResource *SkinResourceCatalog::find(SkinResourceId id) const noexcept { const auto it=resources_.find(id); return it==resources_.end()?nullptr:&it->second; }
const PreparedSkinTextAtlas *SkinResourceCatalog::findTextAtlas(SkinTextAtlasId id) const noexcept { const auto it=atlases_.find(id); return it==atlases_.end()?nullptr:&it->second; }
const PreparedSkinTextAtlas *SkinResourceCatalog::findTextAtlas(const SkinTextAtlasKey &key) const noexcept { const auto it=atlasKeys_.find(key); return it==atlasKeys_.end()?nullptr:findTextAtlas(it->second); }

SkinResourcePreparationService::SkinResourcePreparationService()
    : cache_(SkinResourcePolicy::cacheByteBudget),
      coordinator_([](const image_decode::ImageDecodeRequest &request,
                      std::stop_token stop) -> std::optional<image_decode::DecodedImageData> {
        if (stop.stop_requested() || !request.encoded) return std::nullopt;
        return image_decode::decodeImageMemory(
            std::span<const std::byte>(request.encoded->data(), request.encoded->size()),
            SkinResourcePolicy::maximumDimension,
            SkinResourcePolicy::maximumImageBytes);
      }, SkinResourcePolicy::workerCount) {}

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
    std::lock_guard lock(serviceMutex_);
    if (state_ == State::Running) { state_ = State::Stopping; leader = true; }
  }
  if (leader) coordinator_.shutdown();
  std::unique_lock lock(serviceMutex_);
  serviceCv_.wait(lock, [this] { return activeCalls_ == 0; });
  state_ = State::Stopped;
}

SkinResourceValidationResult SkinResourcePreparationService::validateResources(
    SkinResourceValidationInputs input) {
  SkinResourceValidationResult result;
  if (!beginCall()) { result.diagnostics.push_back(diagnostic("skin.resource.service_stopped", "resource preparation service is stopped")); return result; }
  struct End { SkinResourcePreparationService *service; ~End(){ service->endCall(); } } end{this};
  if (!checkInput(input.entry, input.fileSystem, result.diagnostics) ||
      !runtimeStringsWithinPolicy(input.requiredRuntimeStrings, result.diagnostics)) return result;
  std::set<std::string> resolved;
  std::size_t sessionBytes = 0;
  for (const SkinResourceDefinition &definition : input.model.model.resources) {
    if (input.stop.stop_requested()) { result.cancelled = true; return result; }
    const auto *resource = std::get_if<SkinImageResource>(&definition);
    if (!resource) continue; // Font rasterization is intentionally a later slice.
    const auto candidate = input.fileSystem.resolveResourceCandidates(resource->virtualPath, resource->virtualPath);
    if (!candidate.normalizedVirtualPath) {
      result.diagnostics.push_back(fileDiagnostic(*resource, candidate.failure ? &*candidate.failure : nullptr));
      continue;
    }
    if (!resolved.insert(*candidate.normalizedVirtualPath).second) continue;
    const auto read = input.fileSystem.readResolvedResource(*candidate.normalizedVirtualPath, SkinResourcePolicy::maximumEncodedBytes);
    if (read.failure) { result.diagnostics.push_back(fileDiagnostic(*resource, &*read.failure)); continue; }
    if (read.bytes.size() > SkinResourcePolicy::maximumSessionEncodedBytes - sessionBytes) { result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "encoded resource bytes exceed the session policy")); continue; }
    sessionBytes += read.bytes.size();
    const auto decoded = image_decode::decodeImageMemory(read.bytes, SkinResourcePolicy::maximumDimension, SkinResourcePolicy::maximumImageBytes);
    if (!decoded || !skinResourceDimensionsAllowed(decoded->width, decoded->height, decoded->byteSize())) result.diagnostics.push_back(diagnostic("skin.resource.image_decode_failed", "image decode failed resource validation"));
  }
  result.valid = result.diagnostics.empty();
  return result;
}

SkinResourcePlanResult SkinResourcePreparationService::decodeAndPlan(
    SkinResourcePreparationInputs input) {
  SkinResourcePlanResult result;
  if (!beginCall()) { result.diagnostics.push_back(diagnostic("skin.resource.service_stopped", "resource preparation service is stopped")); return result; }
  struct End { SkinResourcePreparationService *service; ~End(){ service->endCall(); } } end{this};
  if (!checkInput(input.entry, input.fileSystem, result.diagnostics) ||
      !runtimeStringsWithinPolicy(input.requiredRuntimeStrings, result.diagnostics)) return result;
  SkinResourceUploadPlan plan{.revision = std::move(input.revision)};
  std::map<std::string, std::size_t, std::less<>> unique;
  std::size_t encodedBytes = 0;
  for (const SkinResourceDefinition &definition : input.model.model.resources) {
    if (input.stop.stop_requested()) { result.cancelled = true; return result; }
    const auto *resource = std::get_if<SkinImageResource>(&definition);
    if (!resource) continue;
    const auto candidate = input.fileSystem.resolveResourceCandidates(resource->virtualPath, resource->virtualPath);
    if (!candidate.normalizedVirtualPath) { result.diagnostics.push_back(fileDiagnostic(*resource, candidate.failure ? &*candidate.failure : nullptr)); continue; }
    if (const auto found = unique.find(*candidate.normalizedVirtualPath); found != unique.end()) { plan.images[found->second].aliases.push_back(resource->id); continue; }
    if (unique.size() == SkinResourcePolicy::maximumResources) { result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource count exceeds the session policy")); break; }
    const auto read = input.fileSystem.readResolvedResource(*candidate.normalizedVirtualPath, SkinResourcePolicy::maximumEncodedBytes);
    if (read.failure) { result.diagnostics.push_back(fileDiagnostic(*resource, &*read.failure)); continue; }
    if (read.bytes.size() > SkinResourcePolicy::maximumSessionEncodedBytes - encodedBytes) { result.diagnostics.push_back(diagnostic("skin.resource.encoded_limit", "encoded resource bytes exceed the session policy")); break; }
    encodedBytes += read.bytes.size();
    const std::string key = plan.revision.revision().lowercaseSha256 + ":" + *candidate.normalizedVirtualPath;
    std::optional<image_decode::DecodedImageData> decoded;
    { std::lock_guard lock(serviceMutex_); decoded = cache_.get(key); }
    if (!decoded) {
      auto owned = std::make_shared<const std::vector<std::byte>>(std::move(read.bytes));
      const auto ticket = coordinator_.request({.key=key, .path={}, .encoded=std::move(owned)});
      const auto waited = coordinator_.waitTake(ticket, input.stop);
      if (waited.state == image_decode::ImageDecodeWaitState::Cancelled || waited.state == image_decode::ImageDecodeWaitState::Stopped || input.stop.stop_requested()) { result.cancelled = true; return result; }
      if (waited.state != image_decode::ImageDecodeWaitState::Ready || !waited.image) { result.diagnostics.push_back(diagnostic("skin.resource.image_decode_failed", "image decode failed during planning")); continue; }
      decoded = waited.image;
      { std::lock_guard lock(serviceMutex_); cache_.put(key, *decoded); }
    }
    if (!skinResourceDimensionsAllowed(decoded->width, decoded->height, decoded->byteSize()) || decoded->byteSize() > SkinResourcePolicy::maximumSessionDecodedBytes - plan.decodedBytes) { result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "decoded resource bytes exceed the session policy")); break; }
    plan.decodedBytes += decoded->byteSize();
    unique.emplace(*candidate.normalizedVirtualPath, plan.images.size());
    plan.images.push_back({.id=resource->id, .pixels=*decoded});
  }
  if (!result.diagnostics.empty()) return result;
  result.plan = std::move(plan);
  return result;
}
} // namespace skin
#endif
