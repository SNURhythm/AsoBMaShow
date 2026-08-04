#pragma once

#include "BeatorajaSkinModel.h"
#include "../package/SkinTreeSnapshotter.h"
#include "view/DecodedImageCache.h"

#include <array>
#include <map>
#include <memory>
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
  static constexpr std::size_t maximumAtlasBytes = 32U * 1024U * 1024U;
  static constexpr std::size_t maximumAtlasSessionBytes = 128U * 1024U * 1024U;
  static constexpr std::size_t maximumRuntimeStrings = 64;
  static constexpr std::size_t maximumRuntimeStringBytes = 64U * 1024U;
  static constexpr std::size_t maximumGlyphs = 8192;
  static constexpr std::size_t maximumKerningPairs = 16384;
  static constexpr std::size_t cacheByteBudget = 128U * 1024U * 1024U;
  static constexpr std::size_t workerCount = 2;
};

[[nodiscard]] bool skinResourceDimensionsAllowed(int width, int height,
                                                 std::size_t byteCount) noexcept;
[[nodiscard]] bool skinResourceResolveRect(const SkinSourceRect &authored,
                                           int imageWidth, int imageHeight,
                                           SkinSourceRect &resolved) noexcept;

struct SkinDecodedImage {
  SkinResourceId id = 0;
  image_decode::DecodedImageData pixels;
  std::vector<SkinSourceRect> regions;
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
struct SkinResourceUploadPlan { SkinRevisionLease revision; std::vector<SkinDecodedImage> images; std::vector<SkinPreparedGlyphAtlas> atlases; std::size_t decodedBytes = 0; };
struct PreparedSkinResource { SkinResourceId id = 0; bgfx::TextureHandle texture = BGFX_INVALID_HANDLE; int width = 0; int height = 0; std::vector<SkinSourceRect> regions; };
struct PreparedSkinTextAtlas { SkinTextAtlasId id = 0; SkinTextAtlasKey key; bgfx::TextureHandle texture = BGFX_INVALID_HANDLE; int width = 0; int height = 0; std::map<char32_t,SkinPreparedGlyphMetrics> glyphs; std::map<std::pair<char32_t,char32_t>,int> kerning; int ascent = 0; int descent = 0; int lineHeight = 0; };

class SkinTextureDevice {
public:
  virtual ~SkinTextureDevice() = default;
  virtual bgfx::TextureHandle create(const image_decode::DecodedImageData &) = 0;
  virtual void destroy(bgfx::TextureHandle) noexcept = 0;
  virtual bool ownsCurrentThread() const noexcept = 0;
};

class SkinResourceCatalog;
struct SkinResourceUploadResult { std::unique_ptr<SkinResourceCatalog> catalog; std::vector<SkinDiagnostic> diagnostics; };
class SkinResourceCatalog {
public:
  static SkinResourceUploadResult upload(SkinResourceUploadPlan &&, SkinTextureDevice &);
  ~SkinResourceCatalog();
  SkinResourceCatalog(const SkinResourceCatalog &) = delete;
  SkinResourceCatalog &operator=(const SkinResourceCatalog &) = delete;
  const PreparedSkinResource *find(SkinResourceId) const noexcept;
  const PreparedSkinTextAtlas *findTextAtlas(SkinTextAtlasId) const noexcept;
  const PreparedSkinTextAtlas *findTextAtlas(const SkinTextAtlasKey &) const noexcept;
  void enterRenderPhase() noexcept { renderPhase_ = true; }
private:
  struct OwnedTexture { bgfx::TextureHandle handle = BGFX_INVALID_HANDLE; };
  explicit SkinResourceCatalog(SkinRevisionLease &&, SkinTextureDevice &);
  SkinRevisionLease revision_; // declared first: destroyed last
  SkinTextureDevice *device_ = nullptr;
  std::thread::id owner_;
  bool renderPhase_ = false;
  std::vector<OwnedTexture> owned_;
  std::map<SkinResourceId, PreparedSkinResource> resources_;
  std::map<SkinTextAtlasId, PreparedSkinTextAtlas> atlases_;
  std::map<SkinTextAtlasKey, SkinTextAtlasId> atlasKeys_;
};

} // namespace skin
