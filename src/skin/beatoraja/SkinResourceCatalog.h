#pragma once

#include "BeatorajaSkinModel.h"
#include "LuaSkinFileSystem.h"
#include "PomyuCharaResource.h"
#include "SkinDecodeCache.h"
#include "SkinGeneratedTexture.h"
#include "SkinLiveResourceCounters.h"
#include "../SkinSafetyPolicy.h"
#include "../package/SkinTreeSnapshotter.h"
#include "../../view/DecodedImageCache.h"
#include "../../view/ImageDecodeCoordinator.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

#include <bgfx/bgfx.h>

namespace skin {

[[nodiscard]] bool skinResourcePathIsMovie(std::string_view) noexcept;

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
  static constexpr std::size_t maximumGeneratedTextures = 512;
  static constexpr std::size_t maximumGeneratedSessionBytes =
      128U * 1024U * 1024U;
  static constexpr std::size_t maximumMovieDecoders = 8;
  static constexpr std::size_t maximumRuntimeStrings = 64;
  static constexpr std::size_t maximumRuntimeStringBytes = 64U * 1024U;
  static constexpr std::size_t maximumGlyphs = 8192;
  static constexpr std::size_t maximumKerningPairs = 16384;
  // Atlas keys retain exact ordered primary/fallback identity, but must stay
  // small enough to duplicate safely across bounded style variants.
  static constexpr std::size_t maximumFallbackChainDigestBytes =
      64U * 1024U;
  // These CPU-work limits remain fixed even when byte/dimension guards are
  // relaxed: scalable glyph effects perform one blend per covered offset.
  static constexpr double maximumScalableFontOutlineWidth = 8.0;
  static constexpr std::size_t maximumScalableFontPaintBlendOperations =
      64U * 1024U * 1024U;
  static constexpr std::size_t cacheByteBudget = 128U * 1024U * 1024U;
  static constexpr std::size_t workerCount = 6;
};

[[nodiscard]] constexpr std::size_t skinResourceLimit(
    const SkinSafetyPolicy &safetyPolicy,
    std::size_t standardLimit) noexcept {
  return static_cast<std::size_t>(safetyPolicy.limit(
      SkinSafetyGuard::ResourceAllocationLimit, standardLimit));
}

[[nodiscard]] constexpr int skinResourceDimensionLimit(
    const SkinSafetyPolicy &safetyPolicy) noexcept {
  return safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit)
             ? SkinResourcePolicy::maximumDimension
             : std::numeric_limits<int>::max();
}

// This is the one aggregate limit ledger shared by synchronous validation,
// asynchronous planning, and render-thread upload preflight.  Each operation
// is transactional: a rejected increment leaves the ledger unchanged, so an
// optional failed resource cannot poison later accepted resources.
class SkinResourceSessionAccounting {
public:
  explicit SkinResourceSessionAccounting(
      SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{}) noexcept
      : safetyPolicy_(safetyPolicy) {}
  [[nodiscard]] bool addImage(std::size_t physicalResources,
                              std::size_t logicalResources,
                              std::size_t encodedBytes,
                              std::size_t decodedBytes,
                              std::size_t regions) noexcept;
  [[nodiscard]] bool addAtlas(std::size_t decodedBytes, std::size_t glyphs,
                              std::size_t kerningPairs,
                              std::size_t physicalResources = 1,
                              std::size_t scalableFontPaintBlendOperations = 0)
      noexcept;
  [[nodiscard]] std::size_t decodedBytes() const noexcept {
    return decodedBytes_;
  }
  [[nodiscard]] std::size_t encodedBytes() const noexcept {
    return encodedBytes_;
  }
  [[nodiscard]] std::size_t
  remainingScalableFontPaintBlendOperations() const noexcept {
    const std::size_t maximum = skinResourceLimit(
        safetyPolicy_,
        SkinResourcePolicy::maximumScalableFontPaintBlendOperations);
    return maximum - std::min(maximum, scalableFontPaintBlendOperations_);
  }

private:
  SkinSafetyPolicy safetyPolicy_;
  std::size_t physicalResources_ = 0;
  std::size_t logicalResources_ = 0;
  std::size_t encodedBytes_ = 0;
  std::size_t decodedBytes_ = 0;
  std::size_t regions_ = 0;
  std::size_t atlases_ = 0;
  std::size_t atlasBytes_ = 0;
  std::size_t glyphs_ = 0;
  std::size_t kerningPairs_ = 0;
  std::size_t scalableFontPaintBlendOperations_ = 0;
};

[[nodiscard]] bool skinResourceDimensionsAllowed(int width, int height,
                                                 std::size_t byteCount,
                                                 SkinSafetyPolicy safetyPolicy =
                                                     SkinSafetyPolicy{}) noexcept;
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
void resetSkinResourcePlatformAssetReadsForTesting() noexcept;
[[nodiscard]] std::size_t
skinResourcePlatformAssetReadsForTesting() noexcept;
void setSkinResourceAccountingLimitsForTesting(
    std::size_t maximumSessionEncodedBytes,
    std::size_t maximumAtlasSessionBytes) noexcept;
void resetSkinResourceAccountingLimitsForTesting() noexcept;
[[nodiscard]] std::size_t
skinResourceCommittedEncodedBytesForTesting() noexcept;
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
enum class SkinTextLayoutKind : std::uint8_t {
  Scalable,
  Bitmap,
  Lr2Image,
};
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
struct SkinPreparedGlyphMetrics {
  SkinSourceRect region;
  int bearingX = 0;
  int bearingY = 0;
  int advance = 0;
  // Bytecode-equivalent BitmapFontCache bottom offset from the line anchor.
  // It includes the primary layout ascent, the selected face's yoffset, and
  // any bounded atlas padding, so fallback glyphs need no font access later.
  int layoutOffsetY = 0;
  std::size_t page = 0;
  // Bitmap fallback faces retain their own source type so a colored
  // distance-field primary can identify ordinary color glyphs.
  int bitmapFontType = -1;
};
struct SkinPreparedGlyphPage {
  std::string physicalKey;
  std::optional<image_decode::DecodedImageData> pixels;
  int bitmapFontType = 0;
};
struct SkinPreparedGlyphAtlas {
  SkinTextAtlasId id = 0; SkinTextAtlasKey key; image_decode::DecodedImageData pixels;
  std::vector<SkinPreparedGlyphPage> pages;
  std::map<char32_t, SkinPreparedGlyphMetrics> glyphs;
  std::map<std::pair<char32_t,char32_t>, int> kerning;
  int ascent = 0; int capHeight = 0; int descent = 0; int lineHeight = 0;
  bool bitmapFont = false;
  int bitmapFontType = 0;
  int originalSize = 0;
  int pageWidth = 0;
  int pageHeight = 0;
  SkinTextLayoutKind layoutKind = SkinTextLayoutKind::Scalable;
  int margin = 0;
  std::size_t paintBlendOperations = 0;
};
struct SkinResourceUploadPlan {
  SkinRevisionLease revision;
  SkinSafetyPolicy safetyPolicy{};
  std::vector<SkinDecodedImage> images;
  std::map<int, SkinResourceId> builtinImageResources;
  std::vector<SkinPreparedGlyphAtlas> atlases;
  // Render-time text lookup must not reconstruct the configured and securely
  // resolved fallback-chain identity used to key an atlas.
  std::map<SkinObjectId, SkinTextAtlasId> textAtlasesByObject;
  std::vector<PreparedPomyuCharaResource> pomyuCharas;
  std::array<int, 8> pomyuMotionCyclesMillis = {1, 1, 1, 1, 1, 1, 1, 1};
  std::size_t decodedBytes = 0;
};

struct SkinResourceValidationInputs {
  SkinRevisionReadView revision;
  SkinEntryId entry;
  const LuaSkinFileSystem &fileSystem;
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  std::span<const std::string> requiredRuntimeStrings;
  std::map<SkinObjectId, std::vector<std::string>>
      requiredRuntimeStringsByObject;
  bool practiceMode = false;
  SkinSafetyPolicy safetyPolicy{};
  std::stop_token stop;
};
struct SkinResourceValidationResult { bool valid = false; bool cancelled = false; std::vector<SkinDiagnostic> diagnostics; };
using SkinBuiltinImageReader = std::function<bool(
    const std::filesystem::path &, std::vector<unsigned char> &,
    std::size_t, std::string *, std::stop_token)>;
struct SkinResourcePreparationInputs {
  SkinRevisionLease revision;
  SkinEntryId entry;
  const LuaSkinFileSystem &fileSystem;
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  std::span<const std::string> requiredRuntimeStrings;
  std::map<SkinObjectId, std::vector<std::string>>
      requiredRuntimeStringsByObject;
  bool practiceMode = false;
  std::map<int, std::filesystem::path> builtinImagePaths;
  SkinBuiltinImageReader builtinImageReader;
  SkinSafetyPolicy safetyPolicy{};
  std::stop_token stop;
};
struct SkinResourcePlanResult { std::optional<SkinResourceUploadPlan> plan; bool cancelled = false; std::vector<SkinDiagnostic> diagnostics; };

// Selector title changes use the same font preparation as the initial plan,
// but do not revisit images, movies, or unrelated text objects.
struct SkinTextAtlasPreparationInputs {
  SkinRevisionLease revision;
  SkinEntryId entry;
  const LuaSkinFileSystem &fileSystem;
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  std::map<SkinObjectId, std::vector<std::string>>
      requiredRuntimeStringsByObject;
  std::set<SkinObjectId> targetObjects;
  SkinSafetyPolicy safetyPolicy{};
  std::stop_token stop;
};
struct SkinPreparedTextAtlasUpdate {
  SkinPreparedGlyphAtlas atlas;
  // These are the SkinText objects collected into the prepared atlas. The
  // catalog uses their resident bindings rather than assuming an atlas key is
  // globally unique across a pre-existing compatibility catalog.
  std::vector<SkinObjectId> objects;
};
struct SkinTextAtlasUpdatePlan {
  std::vector<SkinPreparedTextAtlasUpdate> atlases;
};
struct SkinTextAtlasPreparationResult {
  std::optional<SkinTextAtlasUpdatePlan> plan;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

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
  SkinTextAtlasPreparationResult
  prepareTextAtlasUpdates(SkinTextAtlasPreparationInputs);
  void shutdown() noexcept;
  [[nodiscard]] SkinDecodeCache &decodeCache() noexcept { return decodeCache_; }
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
  SkinDecodeCache decodeCache_;
  State state_ = State::Running;
  std::stop_source stop_;
  std::size_t activeCalls_ = 0;
  bool shutdownComplete_ = false;
  bool builtInDecoder_ = false;
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
struct PreparedSkinTextAtlas {
  SkinTextAtlasId id = 0;
  SkinTextAtlasKey key;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  int width = 0;
  int height = 0;
  struct Page {
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    int width = 0;
    int height = 0;
    int bitmapFontType = 0;
    bool available = false;
  };
  std::vector<Page> pages;
  std::map<char32_t, SkinPreparedGlyphMetrics> glyphs;
  std::map<std::pair<char32_t, char32_t>, int> kerning;
  int ascent = 0;
  int capHeight = 0;
  int descent = 0;
  int lineHeight = 0;
  bool bitmapFont = false;
  int bitmapFontType = 0;
  int originalSize = 0;
  int pageWidth = 0;
  int pageHeight = 0;
  SkinTextLayoutKind layoutKind = SkinTextLayoutKind::Scalable;
  int margin = 0;
};

struct PreparedSkinGeneratedTexture {
  SkinGeneratedTextureKey key;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  int width = 0;
  int height = 0;
};

class SkinTextureDevice {
public:
  virtual ~SkinTextureDevice() = default;
  virtual bgfx::TextureHandle create(const image_decode::DecodedImageData &) = 0;
  virtual bgfx::TextureHandle create(
      const image_decode::DecodedImageData &image,
      SkinSafetyPolicy) {
    return create(image);
  }
  virtual void destroy(bgfx::TextureHandle) noexcept = 0;
  virtual bool update(bgfx::TextureHandle,
                      const image_decode::DecodedImageData &) {
    return false;
  }
  virtual bool ownsCurrentThread() const noexcept = 0;
  virtual int maximumTextureDimension() const noexcept {
    return SkinResourcePolicy::maximumDimension;
  }
  virtual int maximumTextureDimension(SkinSafetyPolicy) const noexcept {
    return maximumTextureDimension();
  }
};

// Renderers depend only on immutable prepared values.  This deliberately
// excludes upload, decode, path, lease, and device-lifetime operations.
class SkinPreparedResourceView {
public:
  virtual ~SkinPreparedResourceView() = default;
  virtual const PreparedSkinResource *
  find(SkinResourceId) const noexcept = 0;
  virtual const SkinResolvedRegion *
  findResolvedRegion(SkinResourceId,
                     const SkinSourceRect &authored) const noexcept = 0;
  virtual const PreparedSkinTextAtlas *
  findTextAtlas(SkinTextAtlasId) const noexcept = 0;
  virtual const PreparedSkinTextAtlas *
  findTextAtlasForObject(SkinObjectId) const noexcept = 0;
  // System/chart images are prepared outside the package resource plan. A
  // host that owns one exposes the catalog resource which backs the pinned
  // SkinSourceReference ID; unavailable references return nullopt.
  virtual std::optional<SkinResourceId>
  builtinImageResource(int) const noexcept {
    return std::nullopt;
  }
  virtual const PreparedPomyuCharaResource *
  findPomyuChara(SkinObjectId) const noexcept {
    return nullptr;
  }
  virtual const PreparedSkinGeneratedTexture *prepareGeneratedTexture(
      const SkinGeneratedTextureKey &,
      const SkinGeneratedTextureData &) const noexcept {
    return nullptr;
  }
};

class SkinResourceCatalog;
struct SkinResourceUploadResult { std::unique_ptr<SkinResourceCatalog> catalog; std::vector<SkinDiagnostic> diagnostics; };
class SkinResourceCatalog final : public SkinPreparedResourceView {
public:
  static SkinResourceUploadResult upload(SkinResourceUploadPlan &&,
                                         std::shared_ptr<SkinTextureDevice>,
                                         std::shared_ptr<SkinLiveResourceCounters> = {});
  // The catalog must be destroyed on the upload owner thread. Violations
  // fail fast rather than silently leaking GPU resources.
  ~SkinResourceCatalog();
  SkinResourceCatalog(const SkinResourceCatalog &) = delete;
  SkinResourceCatalog &operator=(const SkinResourceCatalog &) = delete;
  const PreparedSkinResource *find(SkinResourceId) const noexcept override;
  const SkinResolvedRegion *findResolvedRegion(SkinResourceId,
                                                const SkinSourceRect &authored) const noexcept override;
  const PreparedSkinTextAtlas *
  findTextAtlas(SkinTextAtlasId) const noexcept override;
  const PreparedSkinTextAtlas *findTextAtlas(const SkinTextAtlasKey &) const noexcept;
  const PreparedSkinTextAtlas *
  findTextAtlasForObject(SkinObjectId) const noexcept override;
  std::optional<SkinResourceId>
  builtinImageResource(int) const noexcept override;
  const PreparedPomyuCharaResource *
  findPomyuChara(SkinObjectId) const noexcept override;
  [[nodiscard]] const std::array<int, 8> &
  pomyuMotionCyclesMillis() const noexcept {
    return pomyuMotionCyclesMillis_;
  }
  [[nodiscard]] std::size_t textureCount() const noexcept {
    return owned_.size();
  }
#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
  [[nodiscard]] std::vector<SkinResourceId>
  preparedResourceIdsForTesting() const;
  [[nodiscard]] std::vector<SkinObjectId>
  preparedTextObjectIdsForTesting() const;
#endif
  const PreparedSkinGeneratedTexture *prepareGeneratedTexture(
      const SkinGeneratedTextureKey &,
      const SkinGeneratedTextureData &) const noexcept override;
  // Beatoraja keeps the selector skin resident while its selected song's
  // stage, back, and banner resources change. These three source references
  // are therefore replaced independently of the authored resource catalog.
  [[nodiscard]] bool replaceBuiltinImage(
      int reference,
      std::optional<image_decode::DecodedImageData> pixels) noexcept;
  // Replaces a resident text atlas on the render owner. The object bindings
  // select the same resident SkinText instances that prepared this refresh.
  [[nodiscard]] bool
  replaceTextAtlas(SkinPreparedGlyphAtlas &&atlas,
                   std::span<const SkinObjectId> objects) noexcept;
  void enterRenderPhase() noexcept { renderPhase_ = true; }
private:
  struct OwnedTexture { bgfx::TextureHandle handle = BGFX_INVALID_HANDLE; };
  explicit SkinResourceCatalog(SkinRevisionLease &&,
                               std::shared_ptr<SkinTextureDevice>,
                               std::shared_ptr<SkinLiveResourceCounters>);
  SkinRevisionLease revision_; // declared first: destroyed last
  // Structural ownership prevents backend destruction before catalog teardown.
  std::shared_ptr<SkinTextureDevice> device_;
  // Retained independently from ApplicationContext so a catalog teardown
  // cannot outlive the app-global ownership accounting it must release.
  std::shared_ptr<SkinLiveResourceCounters> liveCounters_;
  std::thread::id owner_;
  bool renderPhase_ = false;
  bool liveResourceCounted_ = false;
  mutable std::vector<OwnedTexture> owned_;
  std::map<SkinResourceId, PreparedSkinResource> resources_;
  std::map<int, SkinResourceId> builtinImageResources_;
  std::map<SkinTextAtlasId, PreparedSkinTextAtlas> atlases_;
  std::map<SkinTextAtlasKey, SkinTextAtlasId> atlasKeys_;
  std::map<SkinObjectId, SkinTextAtlasId> textAtlasesByObject_;
  std::map<SkinObjectId, PreparedPomyuCharaResource> pomyuCharas_;
  std::array<int, 8> pomyuMotionCyclesMillis_ = {1, 1, 1, 1,
                                                 1, 1, 1, 1};
  struct GeneratedTextureEntry {
    PreparedSkinGeneratedTexture prepared;
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;
    std::uint64_t contentRevision = 0;
    std::size_t ownedIndex = 0;
  };
  mutable std::map<SkinGeneratedTextureKey, GeneratedTextureEntry>
      generatedTextures_;
  mutable std::size_t generatedDecodedBytes_ = 0;
  SkinSafetyPolicy safetyPolicy_{};
};

} // namespace skin
