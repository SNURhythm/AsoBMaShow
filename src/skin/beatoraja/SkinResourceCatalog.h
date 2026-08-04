#pragma once

#include "BeatorajaSkinModel.h"
#include "LuaSkinFileSystem.h"
#include "../package/SkinTreeSnapshotter.h"
#include "../../view/DecodedImageCache.h"
#include "../../view/ImageDecodeCoordinator.h"

#include <array>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

#include <bgfx/bgfx.h>

namespace skin {

// One fixed policy is shared by staged validation and planning. Values are
// deliberately far below package-import limits: resources are frame-critical.
struct SkinResourcePolicy {
  static constexpr std::size_t maximumResources = 512;
  static constexpr std::size_t maximumRegions = 200000;
  static constexpr std::size_t maximumEncodedBytes = 32U * 1024U * 1024U;
  static constexpr std::size_t maximumSessionEncodedBytes = 128U * 1024U * 1024U;
  static constexpr int maximumDimension = 8192;
  static constexpr std::size_t maximumImageBytes = 128U * 1024U * 1024U;
  static constexpr std::size_t maximumSessionDecodedBytes = 256U * 1024U * 1024U;
  static constexpr std::size_t maximumAtlases = 64;
  static constexpr std::size_t maximumTextAtlasUses = 8'192;
  static constexpr std::size_t maximumAtlasBytes = 32U * 1024U * 1024U;
  static constexpr std::size_t maximumAtlasSessionBytes = 128U * 1024U * 1024U;
  static constexpr std::size_t maximumRuntimeStrings = 64;
  static constexpr std::size_t maximumRuntimeStringBytes = 64U * 1024U;
  static constexpr std::size_t maximumGlyphs = 8192;
  static constexpr std::size_t maximumKerningPairs = 16384;
  // Atlas keys retain exact ordered primary/fallback identity, but must stay
  // small enough to duplicate safely across bounded style variants.
  static constexpr std::size_t maximumFallbackChainDigestBytes =
      64U * 1024U;
  static constexpr std::size_t cacheByteBudget = 128U * 1024U * 1024U;
  static constexpr std::size_t workerCount = 2;
};

// This is the one aggregate limit ledger shared by synchronous validation,
// asynchronous planning, and render-thread upload preflight.  Each operation
// is transactional: a rejected increment leaves the ledger unchanged, so an
// optional failed resource cannot poison later accepted resources.
class SkinResourceSessionAccounting {
public:
  [[nodiscard]] bool addImage(std::size_t physicalResources,
                              std::size_t logicalResources,
                              std::size_t encodedBytes,
                              std::size_t decodedBytes,
                              std::size_t regions) noexcept;
  [[nodiscard]] bool addAtlas(std::size_t decodedBytes, std::size_t glyphs,
                              std::size_t kerningPairs) noexcept;
  [[nodiscard]] std::size_t decodedBytes() const noexcept {
    return decodedBytes_;
  }

private:
  std::size_t physicalResources_ = 0;
  std::size_t logicalResources_ = 0;
  std::size_t encodedBytes_ = 0;
  std::size_t decodedBytes_ = 0;
  std::size_t regions_ = 0;
  std::size_t atlases_ = 0;
  std::size_t atlasBytes_ = 0;
  std::size_t glyphs_ = 0;
  std::size_t kerningPairs_ = 0;
};

[[nodiscard]] bool skinResourceDimensionsAllowed(int width, int height,
                                                 std::size_t byteCount) noexcept;
[[nodiscard]] bool skinResourceResolveRect(const SkinSourceRect &authored,
                                           int imageWidth, int imageHeight,
                                           SkinSourceRect &resolved) noexcept;

#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
// The regression suite observes ordered-map comparisons rather than wall
// time, which keeps the maximum-region test deterministic across machines.
void resetSkinResourceRegionIdentityChecksForTesting() noexcept;
[[nodiscard]] std::size_t skinResourceRegionIdentityChecksForTesting() noexcept;
void resetSkinResourceRegionLookupComparisonsForTesting() noexcept;
[[nodiscard]] std::size_t
skinResourceRegionLookupComparisonsForTesting() noexcept;
void resetSkinResourceFontAtlasRequestHighWaterForTesting() noexcept;
[[nodiscard]] std::size_t
skinResourceFontAtlasRequestHighWaterForTesting() noexcept;
#endif

// The authored rectangle is the stable command-side identity. Resolution is
// value-owned during preparation so later command construction never needs to
// repeat model traversal or image-size dependent crop arithmetic.
struct SkinResolvedRegion {
  SkinSourceRect authored;
  SkinSourceRect resolved;
};

struct SkinDecodedImage {
  SkinResourceId id = 0;
  // Multiple model IDs can resolve to one immutable package resource. They
  // share this physical upload; only the catalog owned-texture table owns it.
  std::vector<SkinResourceId> aliases;
  image_decode::DecodedImageData pixels;
  std::vector<SkinSourceRect> regions;
  std::vector<SkinResolvedRegion> regionMappings;
  std::map<SkinResourceId, std::vector<SkinSourceRect>> aliasRegions;
  std::map<SkinResourceId, std::vector<SkinResolvedRegion>> aliasRegionMappings;
};
using SkinTextAtlasId = std::uint32_t;
struct SkinTextAtlasKey {
  SkinResourceId font = 0;
  int pointSize = 0;
  std::array<std::uint8_t, 4> outlineRgba{255, 255, 255, 0};
  double outlineWidth = 0.0;
  std::array<std::uint8_t, 4> shadowRgba{255, 255, 255, 0};
  double shadowOffsetX = 0.0;
  double shadowOffsetY = 0.0;
  double shadowSmoothness = 0.0;
  std::string fallbackChainDigest;
  auto operator<=>(const SkinTextAtlasKey &) const = default;
};
struct SkinPreparedGlyphMetrics { SkinSourceRect region; int bearingX = 0; int bearingY = 0; int advance = 0; };
struct SkinPreparedGlyphAtlas {
  SkinTextAtlasId id = 0; SkinTextAtlasKey key; image_decode::DecodedImageData pixels;
  std::map<char32_t, SkinPreparedGlyphMetrics> glyphs;
  std::map<std::pair<char32_t,char32_t>, int> kerning;
  int ascent = 0; int descent = 0; int lineHeight = 0;
};
struct SkinResourceUploadPlan {
  SkinRevisionLease revision;
  std::vector<SkinDecodedImage> images;
  std::vector<SkinPreparedGlyphAtlas> atlases;
  // Render-time text lookup must not reconstruct the configured and securely
  // resolved fallback-chain identity used to key an atlas.
  std::map<SkinObjectId, SkinTextAtlasId> textAtlasesByObject;
  std::size_t decodedBytes = 0;
};

struct SkinResourceValidationInputs {
  SkinRevisionReadView revision;
  SkinEntryId entry;
  const LuaSkinFileSystem &fileSystem;
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  std::span<const std::string> requiredRuntimeStrings;
  std::stop_token stop;
};
struct SkinResourceValidationResult { bool valid = false; bool cancelled = false; std::vector<SkinDiagnostic> diagnostics; };
struct SkinResourcePreparationInputs {
  SkinRevisionLease revision;
  SkinEntryId entry;
  const LuaSkinFileSystem &fileSystem;
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  std::span<const std::string> requiredRuntimeStrings;
  std::stop_token stop;
};
struct SkinResourcePlanResult { std::optional<SkinResourceUploadPlan> plan; bool cancelled = false; std::vector<SkinDiagnostic> diagnostics; };

class SkinResourcePreparationService {
public:
  using Decoder = std::function<std::optional<image_decode::DecodedImageData>(
      std::span<const std::byte>, std::stop_token)>;
  SkinResourcePreparationService();
  explicit SkinResourcePreparationService(Decoder decoder,
                                          std::size_t workerCount = SkinResourcePolicy::workerCount);
  ~SkinResourcePreparationService();
  SkinResourcePreparationService(const SkinResourcePreparationService &) = delete;
  SkinResourcePreparationService &operator=(const SkinResourcePreparationService &) = delete;
  SkinResourceValidationResult validateResources(SkinResourceValidationInputs);
  SkinResourcePlanResult decodeAndPlan(SkinResourcePreparationInputs);
  void shutdown() noexcept;
private:
  enum class State { Running, Stopping, Stopped };
  bool beginCall();
  void endCall() noexcept;
  [[nodiscard]] bool cancellationRequested(std::stop_token) const noexcept;
  mutable std::mutex serviceMutex_;
  std::condition_variable serviceCv_;
  image_decode::DecodedImageCache cache_;
  Decoder decoder_;
  image_decode::ImageDecodeCoordinator coordinator_;
  State state_ = State::Running;
  std::stop_source stop_;
  std::size_t activeCalls_ = 0;
  bool shutdownComplete_ = false;
};
struct PreparedSkinResource {
  SkinResourceId id = 0;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  int width = 0;
  int height = 0;
  std::vector<SkinSourceRect> regions;
  std::vector<SkinResolvedRegion> regionMappings;
  // Compact immutable index into regionMappings, ordered by authored rect.
  // This preserves authored-use order in regionMappings while making every
  // render-time lookup logarithmic.
  std::vector<std::uint32_t> regionLookupOrder;
};
struct PreparedSkinTextAtlas { SkinTextAtlasId id = 0; SkinTextAtlasKey key; bgfx::TextureHandle texture = BGFX_INVALID_HANDLE; int width = 0; int height = 0; std::map<char32_t,SkinPreparedGlyphMetrics> glyphs; std::map<std::pair<char32_t,char32_t>,int> kerning; int ascent = 0; int descent = 0; int lineHeight = 0; };

class SkinTextureDevice {
public:
  virtual ~SkinTextureDevice() = default;
  virtual bgfx::TextureHandle create(const image_decode::DecodedImageData &) = 0;
  virtual void destroy(bgfx::TextureHandle) noexcept = 0;
  virtual bool ownsCurrentThread() const noexcept = 0;
  virtual int maximumTextureDimension() const noexcept {
    return SkinResourcePolicy::maximumDimension;
  }
};

class SkinResourceCatalog;
struct SkinResourceUploadResult { std::unique_ptr<SkinResourceCatalog> catalog; std::vector<SkinDiagnostic> diagnostics; };
class SkinResourceCatalog {
public:
  static SkinResourceUploadResult upload(SkinResourceUploadPlan &&,
                                         std::shared_ptr<SkinTextureDevice>);
  // The catalog must be destroyed on the upload owner thread. Violations
  // fail fast rather than silently leaking GPU resources.
  ~SkinResourceCatalog();
  SkinResourceCatalog(const SkinResourceCatalog &) = delete;
  SkinResourceCatalog &operator=(const SkinResourceCatalog &) = delete;
  const PreparedSkinResource *find(SkinResourceId) const noexcept;
  const SkinResolvedRegion *findResolvedRegion(SkinResourceId,
                                                const SkinSourceRect &authored) const noexcept;
  const PreparedSkinTextAtlas *findTextAtlas(SkinTextAtlasId) const noexcept;
  const PreparedSkinTextAtlas *findTextAtlas(const SkinTextAtlasKey &) const noexcept;
  const PreparedSkinTextAtlas *
  findTextAtlasForObject(SkinObjectId) const noexcept;
  void enterRenderPhase() noexcept { renderPhase_ = true; }
private:
  struct OwnedTexture { bgfx::TextureHandle handle = BGFX_INVALID_HANDLE; };
  explicit SkinResourceCatalog(SkinRevisionLease &&,
                               std::shared_ptr<SkinTextureDevice>);
  SkinRevisionLease revision_; // declared first: destroyed last
  // Structural ownership prevents backend destruction before catalog teardown.
  std::shared_ptr<SkinTextureDevice> device_;
  std::thread::id owner_;
  bool renderPhase_ = false;
  std::vector<OwnedTexture> owned_;
  std::map<SkinResourceId, PreparedSkinResource> resources_;
  std::map<SkinTextAtlasId, PreparedSkinTextAtlas> atlases_;
  std::map<SkinTextAtlasKey, SkinTextAtlasId> atlasKeys_;
  std::map<SkinObjectId, SkinTextAtlasId> textAtlasesByObject_;
};

} // namespace skin
