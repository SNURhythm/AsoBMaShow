#include "skin/beatoraja/SkinResourceCatalog.h"
#include "skin/beatoraja/SkinMovieCatalog.h"
#include "skin/beatoraja/SkinBitmapFontParser.h"
#include "skin/beatoraja/SkinTextAtlas.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"
#include "view/ImageFileDecoder.h"
#include "view/SdlTtfRuntime.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iostream>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace {
int failures = 0;
void expect(bool value, std::string_view message) { if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }

void testBitmapFontDescriptorParsingMatchesPinnedSources() {
  const auto fixture = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
                       "tests/fixtures/beatoraja_skin/resources/bitmap-font/fixture.fnt";
  std::ifstream input(fixture, std::ios::binary);
  const std::string encoded((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  const auto bytes = std::as_bytes(std::span(encoded));
  const auto parsed = skin::parseSkinBitmapFont(
      skin::SkinBitmapFontResource{.id=7,
                                   .virtualPath="resources/bitmap-font/fixture.fnt",
                                   .type=2,
                                   .originalSize=0,
                                   .authoredOrdinal=4},
      bytes, skin::SkinBitmapFontSourceFormat::BmFont);
  expect(parsed.font && parsed.error.empty() &&
             parsed.font->resource.id == 7 &&
             parsed.font->resource.type == 2 &&
             parsed.font->resource.originalSize == 10 &&
             parsed.font->pageWidth == 40 && parsed.font->pageHeight == 20 &&
             parsed.font->lineHeight == 12 && parsed.font->base == 9 &&
             parsed.font->pagePaths ==
                 std::vector<std::string>{"page.png", "page.png"},
         "text BMFont info/common/page records retain original size, metrics, type, and ordered pages");
  if (parsed.font) {
    const auto a = parsed.font->glyphs.find(U'A');
    const auto supplementary = parsed.font->glyphs.find(U'\U0001f642');
    expect(a != parsed.font->glyphs.end() &&
               a->second.page == 0 && a->second.region.x == 9 &&
               a->second.region.y == 0 && a->second.region.w == 6 &&
               a->second.region.h == 8 &&
               a->second.xOffset == 1 && a->second.yOffset == 1 &&
               a->second.xAdvance == 7 &&
               supplementary != parsed.font->glyphs.end() &&
               supplementary->second.page == 1 &&
               parsed.font->kerning.at({U'A', U'V'}) == -2,
           "BMFont glyph rectangles, supplementary code points, pages, advances, and kerning are typed values");
  }

  std::string mismatchedCounts = encoded;
  const auto glyphCount = mismatchedCounts.find("chars count=6");
  const auto kerningCount = mismatchedCounts.find("kernings count=1");
  expect(glyphCount != std::string::npos &&
             kerningCount != std::string::npos,
         "BMFont count compatibility fixture contains both advisory counts");
  if (glyphCount != std::string::npos &&
      kerningCount != std::string::npos) {
    mismatchedCounts.replace(glyphCount, std::string_view("chars count=6").size(),
                             "chars count=5");
    mismatchedCounts.replace(
        kerningCount, std::string_view("kernings count=1").size(),
        "kernings count=0");
  }
  const auto mismatched = skin::parseSkinBitmapFont(
      skin::SkinBitmapFontResource{.id = 8,
                                   .virtualPath = "mismatched-counts.fnt"},
      std::as_bytes(std::span(mismatchedCounts)),
      skin::SkinBitmapFontSourceFormat::BmFont);
  expect(mismatched.font && mismatched.font->glyphs.size() == 6 &&
             mismatched.font->kerning.size() == 1,
         "BMFont advisory count metadata is recomputed like Beatoraja's "
         "SkinTextBitmap remapping path");

  constexpr std::string_view sourceValidWithoutAuxiliaryMetrics =
      "info face=fixture size=12 padding=0,0,0,0\n"
      "common lineHeight=12 base=9 pages=0\n"
      "page file=page.png\n"
      "chars count=1\n"
      "char id=65 x=0 y=0 width=6 height=8 xoffset=1 yoffset=2 "
      "xadvance=7 page=0 chnl=15\n";
  const auto fallbackMetrics = skin::parseSkinBitmapFont(
      skin::SkinBitmapFontResource{.id = 10,
                                   .virtualPath = "fallback-metrics.fnt"},
      std::as_bytes(std::span(sourceValidWithoutAuxiliaryMetrics)),
      skin::SkinBitmapFontSourceFormat::BmFont);
  expect(fallbackMetrics.font &&
             !fallbackMetrics.font->auxiliaryMetricsComplete &&
             fallbackMetrics.font->pagePaths ==
                 std::vector<std::string>{"page.png"},
         "a libGDX-valid BMFont accepts omitted scale metrics and a page id, "
         "then uses Beatoraja's decoded-page metric fallback");

  const std::array<std::byte, 8> binary{
      std::byte{'B'}, std::byte{'M'}, std::byte{'F'}, std::byte{3},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
  const auto rejected = skin::parseSkinBitmapFont(
      skin::SkinBitmapFontResource{.id=1, .virtualPath="binary.fnt"},
      binary, skin::SkinBitmapFontSourceFormat::BmFont);
  expect(!rejected.font &&
             rejected.error == "binary BMFont descriptors are unsupported",
         "the pinned text-only Beatoraja path rejects binary BMFont bytes");

  constexpr std::string_view lr2 =
      "#S,10\r\n"
      "#M,2\r\n"
      "#T,0,page.png\r\n"
      "#R,65,0,1,2,6,8\r\n"
      "#R,288,0,9,2,7,8\r\n";
  const auto lr2Parsed = skin::parseSkinBitmapFont(
      skin::SkinBitmapFontResource{.id=9,
                                   .virtualPath="font/fixture.lr2font",
                                   .type=0,
                                   .authoredOrdinal=8},
      std::as_bytes(std::span(lr2)),
      skin::SkinBitmapFontSourceFormat::Lr2Font);
  expect(lr2Parsed.font && lr2Parsed.font->resource.originalSize == 10 &&
             lr2Parsed.font->margin == 2 &&
             lr2Parsed.font->pagePaths == std::vector<std::string>{"page.png"} &&
             lr2Parsed.font->glyphs.at(U'A').region.x == 1 &&
             lr2Parsed.font->glyphs.at(U'A').region.y == 2 &&
             lr2Parsed.font->glyphs.at(U'A').region.w == 6 &&
             lr2Parsed.font->glyphs.at(U'A').region.h == 8 &&
             lr2Parsed.font->glyphs.at(U'\u301c').region.x == 9 &&
             lr2Parsed.font->glyphs.at(U'\uff5e').region.x == 9,
         "LR2FONT S/M/T/R records share typed pages and glyphs while code 288 maps to both wave-dash forms");
}

void testInstalledSelectorBmFontsWhenRequested() {
  const char *root = std::getenv("ASOBMASHOW_SKIN_ACCEPTANCE_ROOT");
  if (root == nullptr || *root == '\0') {
    return;
  }
  const std::filesystem::path select =
      std::filesystem::path(root) / "LITONE12/Select";
  for (const std::filesystem::path &relative : {
           std::filesystem::path("font/title/lightblue/title.fnt"),
           std::filesystem::path("font/bartitle/lightblue/title.fnt"),
       }) {
    const std::filesystem::path path = select / relative;
    std::ifstream input(path, std::ios::binary);
    const std::string descriptor{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
    expect(input.good() || !descriptor.empty(),
           "installed LITONE12 BMFont descriptor is readable");
    if (descriptor.empty()) {
      continue;
    }
    const auto parsed = skin::parseSkinBitmapFont(
        skin::SkinBitmapFontResource{.id = 1,
                                     .virtualPath = path.generic_string()},
        std::as_bytes(std::span(descriptor)),
        skin::SkinBitmapFontSourceFormat::BmFont,
        skin::SkinSafetyPolicy(skin::SkinSafetyLevel::Unrestricted,
                               std::numeric_limits<std::uint64_t>::max(), true));
    expect(parsed.font.has_value(),
           "installed LITONE12 selector BMFont follows Beatoraja acceptance");
  }
}

image_decode::DecodedImageData bitmapPage(int width, int height,
                                          unsigned char value) {
  return {.width = width,
          .height = height,
          .rgba = std::make_shared<std::vector<unsigned char>>(
              static_cast<std::size_t>(width) *
                  static_cast<std::size_t>(height) * 4U,
              value)};
}

void testBitmapFontsKeepSourceValidMetricsPagesAndMissingGlyphs() {
  constexpr std::string_view noInfoSize =
      "info face=fixture\n"
      "common lineHeight=12 base=9 scaleW=40 scaleH=20 pages=1\n"
      "page id=0 file=page.png\n"
      "chars count=1\n"
      "char id=65 x=0 y=0 width=6 height=8 xoffset=1 yoffset=2 "
      "xadvance=7 page=0 chnl=15\n";
  auto primary = skin::parseSkinBitmapFont(
      skin::SkinBitmapFontResource{.id = 1, .virtualPath = "primary.fnt"},
      std::as_bytes(std::span(noInfoSize)),
      skin::SkinBitmapFontSourceFormat::BmFont);
  expect(primary.font && primary.font->resource.originalSize == 12 &&
             !primary.font->auxiliaryMetricsComplete,
         "a libGDX-valid BMFont without an auxiliary info size keeps the "
         "line-height metric fallback");
  if (!primary.font) {
    return;
  }

  auto fallback = *primary.font;
  fallback.resource.type = 0;
  fallback.resource.virtualPath = "missing-fallback.fnt";
  fallback.glyphs.clear();
  fallback.glyphs.emplace(
      U'B', skin::SkinBitmapGlyph{.codepoint = U'B',
                                 .page = 0,
                                 .region = {.x = 0, .y = 0, .w = 5, .h = 8},
                                 .xAdvance = 6});

  const std::vector<skin::SkinTextAtlasBitmapFace> faces{
      {.font = *primary.font,
       .pages = {{.physicalKey = "page-a",
                  .pixels = bitmapPage(8192, 8, 0x11)}}},
      {.font = fallback,
       .pages = {{.physicalKey = "missing-page", .pixels = std::nullopt}}}};
  const auto built = skin::buildSkinBitmapTextAtlas(
      1,
      {.font = 1, .pointSize = 1, .fallbackChainDigest = "bitmap-chain"},
      faces, std::set<char32_t>{U'A', U'B', U'X'},
      std::set<std::pair<char32_t, char32_t>>{{U'A', U'X'},
                                              {U'X', U'A'}});
  expect(built.atlas && built.error.empty() &&
             built.atlas->layoutKind == skin::SkinTextLayoutKind::Bitmap &&
             built.atlas->pages.size() == 2 &&
             built.atlas->pages.front().pixels &&
             !built.atlas->pages.back().pixels &&
             built.atlas->pixels.rgba == nullptr &&
             built.atlas->pageWidth == 8192 &&
             built.atlas->pageHeight == 8 &&
             built.atlas->glyphs.contains(U'A') &&
             !built.atlas->glyphs.contains(U'B') &&
             !built.atlas->glyphs.contains(U'X') &&
             built.atlas->kerning.empty() &&
             built.atlas->glyphs.at(U'A').layoutOffsetY == -10,
         "bitmap pages remain separate, an unavailable fallback page is "
         "skipped, actual first-page metrics replace the failed auxiliary "
         "scan, a glyph without a replacement is simply absent, and the "
         "baseline offset is the glyph's yOffset plus its ink height "
         "(Beatoraja BitmapFont layout)");

  auto secondPageFont = *primary.font;
  secondPageFont.pagePaths = {"wide-a.png", "wide-b.png"};
  secondPageFont.glyphs.emplace(
      U'B', skin::SkinBitmapGlyph{.codepoint = U'B',
                                 .page = 1,
                                 .region = {.x = 0, .y = 0, .w = 5, .h = 1},
                                 .xAdvance = 5});
  const std::vector<skin::SkinTextAtlasBitmapFace> wideFaces{{
      .font = std::move(secondPageFont),
      .pages = {{.physicalKey = "wide-a", .pixels = bitmapPage(8192, 8, 1)},
                {.physicalKey = "wide-b", .pixels = bitmapPage(8192, 8, 2)}}}};
  const auto wide = skin::buildSkinBitmapTextAtlas(
      2, {.font = 1, .pointSize = 1, .fallbackChainDigest = "wide-pages"},
      wideFaces, std::set<char32_t>{U'A', U'B'},
      std::set<std::pair<char32_t, char32_t>>{});
  expect(wide.atlas && wide.atlas->pages.size() == 2 &&
             wide.atlas->glyphs.at(U'A').page == 0 &&
             wide.atlas->glyphs.at(U'B').page == 1,
         "two individually valid maximum-width BMFont pages do not become "
         "an invalid horizontal mega-atlas");
}

void testBoundedPngAndJpegDecodeBeforeAllocation() {
  const std::filesystem::path resources =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "tests/fixtures/beatoraja_skin/resources";
  const auto png = image_decode::decodeImageFile(resources / "fixture.png", 40, 3200);
  const auto jpg = image_decode::decodeImageFile(resources / "fixture.jpg", 40, 3200);
  expect(png && png->width == 40 && png->height == 20 && png->byteSize() == 3200,
         "purpose-built PNG decodes to bounded RGBA");
  expect(jpg && jpg->width == 40 && jpg->height == 20 && jpg->byteSize() == 3200,
         "purpose-built JPEG decodes to bounded RGBA");
  expect(!image_decode::decodeImageFile(resources / "fixture.png", 39, 3200),
         "header dimensions exceeding policy are rejected before full decode");
  const auto oversized = std::filesystem::temp_directory_path() / "asobmashow-task13-encoded-limit.bin";
  { std::ofstream stream(oversized, std::ios::binary); stream.seekp(4095); stream.put('\0'); }
  expect(!image_decode::decodeImageFile(oversized, 40, 3200, 16),
         "file decoder rejects encoded bytes before allocating a read buffer");
  std::error_code removeError;
  std::filesystem::remove(oversized, removeError);
}

void testSharedSdlTtfRuntimeFinalRelease() {
  expect(text_runtime::acquire() && text_runtime::acquire() &&
             text_runtime::activeReferencesForTesting() == 2,
         "overlapping SDL_ttf owners retain one process runtime");
  text_runtime::release();
  expect(text_runtime::activeReferencesForTesting() == 1,
         "releasing one SDL_ttf owner cannot quit the shared runtime");
  text_runtime::release();
  expect(text_runtime::activeReferencesForTesting() == 0,
         "the final SDL_ttf owner releases only after prior operations close");
}

void testSpriteBoundsAndNormalizedGridCells() {
  skin::SkinSourceRect authored{.x=0,.y=0,.w=40,.h=20,.gridColumn=2,.gridRow=0,.gridColumns=4,.gridRows=2};
  skin::SkinSourceRect resolved;
  expect(skin::skinResourceResolveRect(authored, 40, 20, resolved) &&
             resolved.x == 20 && resolved.y == 0 && resolved.w == 10 && resolved.h == 10,
         "row-major sprite preparation resolves the third grid cell for UV 0.5 to 0.75");
  authored.x = 31;
  expect(skin::skinResourceResolveRect(authored, 40, 20, resolved) &&
             resolved.x == 51 && resolved.y == 0 && resolved.w == 10 &&
             resolved.h == 10,
         "Beatoraja TextureRegion construction preserves crops beyond the decoded image");
  authored = {.x=0,.y=0,.w=-1,.h=-1,.gridColumn=1,.gridRow=1,.gridColumns=2,.gridRows=2};
  expect(skin::skinResourceResolveRect(authored, 40, 20, resolved) &&
             resolved.x == 20 && resolved.y == 10 && resolved.w == 20 && resolved.h == 10,
         "negative-one source dimensions resolve against decoded image bounds before UV preparation");
  authored = {.x = std::numeric_limits<int>::max(),
              .y = 0,
              .w = 2,
              .h = 1,
              .gridColumn = 1,
              .gridColumns = 2,
              .gridRows = 1};
  expect(!skin::skinResourceResolveRect(authored, 40, 20, resolved),
         "sprite-grid resolution rejects signed coordinate overflow");
}

void testTextAtlasKeyRejectsNegativePaintExtents() {
  skin::SkinTextAtlasKey key{
      .font=1, .pointSize=16, .fallbackChainDigest="1:0|8:font.ttf:0"};
  key.outlineWidth = -0.25;
  expect(!skin::canonicalizeSkinTextAtlasKey(key),
         "negative outline extent cannot become a canonical atlas key");
  key.outlineWidth = 8.0;
  expect(skin::canonicalizeSkinTextAtlasKey(key),
         "the maximum ordinary scalable-font outline remains compatible");
  key.outlineWidth = 8.25;
  expect(!skin::canonicalizeSkinTextAtlasKey(key),
         "an oversized scalable-font outline is rejected before raster work");
  expect(skin::canonicalizeSkinTextAtlasKey(
             key, skin::SkinSafetyPolicy(skin::SkinSafetyLevel::Unrestricted)),
         "Unrestricted adds no host outline-width budget");
  key.outlineWidth = 0.0;
  key.shadowSmoothness = -0.25;
  expect(!skin::canonicalizeSkinTextAtlasKey(key),
         "negative shadow smoothing cannot become a canonical atlas key");
  key.shadowSmoothness = 0.0;
  key.fallbackChainDigest.clear();
  expect(!skin::canonicalizeSkinTextAtlasKey(key),
         "an empty fallback-chain identity cannot become an atlas key");
  key.fallbackChainDigest.assign(65537, 'x');
  expect(!skin::canonicalizeSkinTextAtlasKey(key),
         "an oversized fallback-chain identity cannot become an atlas key");

  const std::vector<skin::SkinFontFallbackResource> forward = {
      {.virtualPath="font/a.ttf", .type=0},
      {.virtualPath="font/b.ttf", .type=1}};
  const std::vector<skin::SkinFontFallbackResource> reverse(
      forward.rbegin(), forward.rend());
  expect(skin::stableFallbackChainDigest(1, 0, forward) !=
             skin::stableFallbackChainDigest(1, 0, reverse),
         "fallback-chain identity preserves exact ordered path and type data");
  const std::vector<skin::SkinFontFallbackResource> withEmpty = {
      {.virtualPath="", .type=9},
      {.virtualPath="font/a.ttf", .type=0}};
  const std::vector<skin::SkinFontFallbackResource> withoutEmpty = {
      {.virtualPath="font/a.ttf", .type=0}};
  expect(skin::stableFallbackChainDigest(1, 0, withEmpty) ==
             skin::stableFallbackChainDigest(1, 0, withoutEmpty),
         "empty fallback placeholders do not change public atlas identity");
  std::vector<skin::SkinFontFallbackResource> oversizedChain(
      8192, {.virtualPath="font/fallback.ttf", .type=0});
  expect(skin::stableFallbackChainDigest(1, 0, oversizedChain).empty(),
         "the public fallback-chain builder refuses an oversized identity");
}

void testScalableFontOutlineWorkIsBounded() {
  const std::filesystem::path fontPath =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
      "bgfx/bgfx/examples/runtime/font/signika-regular.ttf";
  std::ifstream input(fontPath, std::ios::binary);
  const std::string encoded((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  const auto encodedBytes = std::as_bytes(std::span(encoded));
  const std::vector<skin::SkinTextAtlasFontBytes> faces{{
      .encoded = std::vector<std::byte>(encodedBytes.begin(),
                                       encodedBytes.end())}};
  const auto ordinary = skin::buildSkinTextAtlas(
      1,
      {.font = 1,
       .pointSize = 24,
       .fallbackChainDigest = "outline-fixture",
       .outlineRgba = {255, 0, 0, 255},
       .outlineWidth = 1.25},
      faces, std::set<char32_t>{U'A'}, {});
  expect(ordinary.atlas && ordinary.error.empty(),
         "an ordinary fractional outline still produces a scalable glyph atlas");

  const skin::SkinTextAtlasKey transparentShadowKey{
      .font = 1,
      .pointSize = 24,
      .shadowRgba = {0, 0, 255, 0},
      .shadowOffsetX = 3.0,
      .shadowOffsetY = 2.0,
      .shadowSmoothness = 1.0,
      .fallbackChainDigest = "transparent-shadow-fixture"};
  const auto transparentShadow = skin::buildSkinTextAtlas(
      5, transparentShadowKey, faces, std::set<char32_t>{U'A'}, {},
      skin::SkinSafetyPolicy{}, /*maximumPaintBlendOperations=*/0);
  expect(transparentShadow.atlas && transparentShadow.error.empty() &&
             transparentShadow.atlas->paintBlendOperations == 0,
         "a transparent shadow consumes no scalable-font paint budget");
  auto transparentOutlineKey = transparentShadowKey;
  transparentOutlineKey.outlineWidth = 8.0;
  transparentOutlineKey.fallbackChainDigest = "transparent-outline-fixture";
  const auto transparentOutline = skin::buildSkinTextAtlas(
      9, transparentOutlineKey, faces, std::set<char32_t>{U'A'}, {},
      skin::SkinSafetyPolicy{}, /*maximumPaintBlendOperations=*/0);
  expect(transparentOutline.atlas && transparentOutline.error.empty() &&
             transparentOutline.atlas->paintBlendOperations == 0,
         "a transparent outline consumes no scalable-font paint budget");

  auto shadowCenterKey = transparentShadowKey;
  shadowCenterKey.shadowRgba[3] = 255;
  shadowCenterKey.shadowSmoothness = 0.0;
  shadowCenterKey.fallbackChainDigest = "shadow-center-fixture";
  const auto rejectedShadowCenter = skin::buildSkinTextAtlas(
      6, shadowCenterKey, faces, std::set<char32_t>{U'A'}, {},
      skin::SkinSafetyPolicy{}, /*maximumPaintBlendOperations=*/0);
  expect(!rejectedShadowCenter.atlas &&
             rejectedShadowCenter.error ==
                 "font paint work exceeds atlas preparation limit",
         "an active shadow center is rejected when no session work remains");
  const auto shadowCenter = skin::buildSkinTextAtlas(
      7, shadowCenterKey, faces, std::set<char32_t>{U'A'}, {});
  auto smoothShadowKey = shadowCenterKey;
  smoothShadowKey.shadowSmoothness = 1.0;
  smoothShadowKey.fallbackChainDigest = "smooth-shadow-fixture";
  const auto smoothShadow = skin::buildSkinTextAtlas(
      8, smoothShadowKey, faces, std::set<char32_t>{U'A'}, {});
  expect(shadowCenter.atlas && smoothShadow.atlas &&
             shadowCenter.atlas->paintBlendOperations > 0 &&
             smoothShadow.atlas->paintBlendOperations ==
                 shadowCenter.atlas->paintBlendOperations * 9U,
         "radius-one shadow smoothing charges its center and eight neighbors");
  const auto oversized = skin::buildSkinTextAtlas(
      4,
      {.font = 1,
       .pointSize = 24,
       .fallbackChainDigest = "outline-width-fixture",
       .outlineRgba = {255, 0, 0, 255},
       .outlineWidth = 8.25},
      faces, std::set<char32_t>{U'A'}, {});
  expect(!oversized.atlas &&
             oversized.error ==
                 "font outline width exceeds atlas preparation limit",
         "oversized scalable outlines report their rejected work explicitly");

  std::set<char32_t> printableAscii;
  for (char32_t codepoint = U'!'; codepoint <= U'~'; ++codepoint) {
    printableAscii.insert(codepoint);
  }
  const auto excessive = skin::buildSkinTextAtlas(
      2,
      {.font = 1,
       .pointSize = 192,
       .fallbackChainDigest = "outline-work-fixture",
       .outlineRgba = {255, 0, 0, 255},
       .outlineWidth = 8.0},
      faces, printableAscii, {});
  if (excessive.error !=
      "font outline work exceeds atlas preparation limit") {
    std::cerr << "outline work diagnostic: "
              << (excessive.atlas ? "atlas published" : excessive.error)
              << '\n';
  }
  expect(!excessive.atlas &&
             excessive.error ==
                 "font outline work exceeds atlas preparation limit",
         "aggregate cap-compliant outline work is rejected before painting");
  const auto attemptedOverride = skin::buildSkinTextAtlas(
      10,
      {.font = 1,
       .pointSize = 192,
       .fallbackChainDigest = "outline-override-fixture",
       .outlineRgba = {255, 0, 0, 255},
       .outlineWidth = 8.0},
      faces, printableAscii, {}, skin::SkinSafetyPolicy{},
      std::numeric_limits<std::size_t>::max());
  expect(!attemptedOverride.atlas &&
             attemptedOverride.error ==
                 "font outline work exceeds atlas preparation limit",
         "a caller cannot relax the fixed scalable-font paint-work safety cap");

  skin::resetSkinTextAtlasPaintBlendOperationsForTesting();
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto lateGlyphFailure = skin::buildSkinTextAtlas(
        11 + static_cast<skin::SkinTextAtlasId>(attempt),
        {.font = 1,
         .pointSize = 24,
         .fallbackChainDigest = "late-glyph-fixture-" +
                                std::to_string(attempt),
         .outlineRgba = {255, 0, 0, 255},
         .outlineWidth = 2.0},
        faces, std::set<char32_t>{U'A', U'\U0010ffff'}, {});
    expect(!lateGlyphFailure.atlas &&
               lateGlyphFailure.error ==
                   "font atlas has an unsupported glyph",
           "a late unsupported glyph rejects the whole scalable atlas");
  }
  expect(skin::skinTextAtlasPaintBlendOperationsForTesting() == 0,
         "repeated late glyph rejection performs no outline blending");

  std::size_t remainingAttemptWork =
      skin::SkinResourcePolicy::maximumScalableFontPaintBlendOperations;
  const auto reserveAttemptWork = [&remainingAttemptWork](std::size_t work) {
    if (work > remainingAttemptWork) return false;
    remainingAttemptWork -= work;
    return true;
  };
  const skin::SkinTextAtlasKey expensiveKey{
      .font = 1,
      .pointSize = 160,
      .outlineRgba = {255, 0, 0, 255},
      .outlineWidth = 8.0,
      .fallbackChainDigest = "post-accounting-fixture"};
  skin::resetSkinTextAtlasPaintBlendOperationsForTesting();
  const auto firstDiscarded = skin::buildSkinTextAtlas(
      13, expensiveKey, faces, printableAscii, {}, skin::SkinSafetyPolicy{},
      skin::SkinResourcePolicy::maximumScalableFontPaintBlendOperations, {},
      reserveAttemptWork);
  const std::size_t workAfterFirst =
      skin::skinTextAtlasPaintBlendOperationsForTesting();
  auto secondKey = expensiveKey;
  secondKey.fallbackChainDigest = "post-accounting-fixture-second";
  const auto secondDiscarded = skin::buildSkinTextAtlas(
      14, secondKey, faces, printableAscii, {}, skin::SkinSafetyPolicy{},
      skin::SkinResourcePolicy::maximumScalableFontPaintBlendOperations, {},
      reserveAttemptWork);
  expect(firstDiscarded.atlas &&
             firstDiscarded.atlas->paintBlendOperations >
                 skin::SkinResourcePolicy::
                         maximumScalableFontPaintBlendOperations /
                     2U,
         "a valid expensive atlas can consume more than half the attempt budget");
  expect(!secondDiscarded.atlas &&
             secondDiscarded.error ==
                 "font outline work exceeds atlas preparation limit" &&
             skin::skinTextAtlasPaintBlendOperationsForTesting() ==
                 workAfterFirst,
         "discarding a painted atlas does not return its monotonic attempt "
         "reservation to a repeated optional request");

  int cancellationChecks = 0;
  const auto cancelled = skin::buildSkinTextAtlas(
      3,
      {.font = 1,
       .pointSize = 24,
       .fallbackChainDigest = "outline-cancellation-fixture",
       .outlineRgba = {255, 0, 0, 255},
       .outlineWidth = 2.0},
      faces, std::set<char32_t>{U'A'}, {}, skin::SkinSafetyPolicy{},
      skin::SkinResourcePolicy::maximumScalableFontPaintBlendOperations,
      [&] { return ++cancellationChecks == 2; });
  expect(!cancelled.atlas &&
             cancelled.error == "font atlas preparation cancelled" &&
             cancellationChecks == 2,
         "scalable outline painting observes deterministic mid-glyph cancellation");
}

void testSharedSessionAccountingRejectsDistributedAggregateOverages() {
  skin::SkinResourceSessionAccounting resources;
  expect(resources.addImage(/*physical=*/1, /*logical=*/1,
                            /*encodedBytes=*/0, /*decodedBytes=*/0,
                            /*regions=*/0),
         "the first physical/logical resource is within the shared session policy");
  for (std::size_t index = 1;
       index < skin::SkinResourcePolicy::maximumResources;
       ++index) {
    expect(resources.addImage(/*physical=*/0, /*logical=*/1,
                              /*encodedBytes=*/0, /*decodedBytes=*/0,
                              /*regions=*/0),
           "distributed aliases remain within the shared logical-resource policy");
  }
  expect(!resources.addImage(/*physical=*/0, /*logical=*/1,
                             /*encodedBytes=*/0, /*decodedBytes=*/0,
                             /*regions=*/0),
         "the distributed 513th logical resource is rejected by shared accounting");

  skin::SkinResourceSessionAccounting regions;
  expect(regions.addImage(/*physical=*/0, /*logical=*/0,
                          /*encodedBytes=*/0, /*decodedBytes=*/0,
                          skin::SkinResourcePolicy::maximumRegions),
         "aggregate regions can exactly meet the shared session policy");
  expect(!regions.addImage(/*physical=*/0, /*logical=*/0,
                           /*encodedBytes=*/0, /*decodedBytes=*/0,
                           /*regions=*/1),
         "one aggregate alias region beyond the policy is rejected");

  skin::SkinResourceSessionAccounting atlasCount;
  for (std::size_t index = 0; index < skin::SkinResourcePolicy::maximumAtlases;
       ++index) {
    expect(atlasCount.addAtlas(/*decodedBytes=*/0, /*glyphs=*/0,
                               /*kerningPairs=*/0),
           "each allowed atlas is accepted by shared accounting");
  }
  expect(!atlasCount.addAtlas(/*decodedBytes=*/0, /*glyphs=*/0,
                              /*kerningPairs=*/0),
         "the aggregate atlas-count overage is rejected");

  skin::SkinResourceSessionAccounting combinedBytes;
  const std::size_t imageBytes =
      skin::SkinResourcePolicy::maximumSessionDecodedBytes - 1;
  expect(combinedBytes.addImage(/*physical=*/1, /*logical=*/1,
                                /*encodedBytes=*/0, imageBytes,
                                /*regions=*/0),
         "image decoded bytes can consume all but one byte of the shared budget");
  expect(!combinedBytes.addAtlas(/*decodedBytes=*/2, /*glyphs=*/0,
                                 /*kerningPairs=*/0),
         "atlas decoded bytes share the image session budget");

  skin::SkinResourceSessionAccounting fontMetadata;
  expect(fontMetadata.addAtlas(/*decodedBytes=*/0,
                               skin::SkinResourcePolicy::maximumGlyphs,
                               skin::SkinResourcePolicy::maximumKerningPairs),
         "aggregate glyph and kerning totals can exactly meet the shared policy");
  expect(!fontMetadata.addAtlas(/*decodedBytes=*/0, /*glyphs=*/1,
                                /*kerningPairs=*/0),
         "an aggregate glyph overage is rejected before upload");
  skin::SkinResourceSessionAccounting pairs;
  expect(pairs.addAtlas(/*decodedBytes=*/0, /*glyphs=*/0,
                        skin::SkinResourcePolicy::maximumKerningPairs) &&
             !pairs.addAtlas(/*decodedBytes=*/0, /*glyphs=*/0,
                             /*kerningPairs=*/1),
         "an aggregate kerning overage is rejected before upload");

  skin::SkinResourceSessionAccounting paintWork;
  constexpr std::size_t firstAtlasPaintWork = 40U * 1024U * 1024U;
  constexpr std::size_t finalAtlasPaintWork = 24U * 1024U * 1024U;
  expect(paintWork.addAtlas(/*decodedBytes=*/0, /*glyphs=*/0,
                            /*kerningPairs=*/0,
                            /*physicalResources=*/1,
                            firstAtlasPaintWork) &&
             paintWork.remainingScalableFontPaintBlendOperations() ==
                 finalAtlasPaintWork,
         "the first scalable atlas commits paint work to the session ledger");
  expect(!paintWork.addAtlas(/*decodedBytes=*/0, /*glyphs=*/0,
                             /*kerningPairs=*/0,
                             /*physicalResources=*/1,
                             finalAtlasPaintWork + 1U) &&
             paintWork.remainingScalableFontPaintBlendOperations() ==
                 finalAtlasPaintWork,
         "a rejected second atlas leaks no transactional paint budget");
  expect(paintWork.addAtlas(/*decodedBytes=*/0, /*glyphs=*/0,
                            /*kerningPairs=*/0,
                            /*physicalResources=*/1,
                            finalAtlasPaintWork) &&
             paintWork.remainingScalableFontPaintBlendOperations() == 0,
         "multiple accepted atlases can consume the exact session paint budget");

  skin::SkinResourceSessionAccounting unrestrictedPaint{
      skin::SkinSafetyPolicy(skin::SkinSafetyLevel::Unrestricted)};
  expect(unrestrictedPaint.addAtlas(
             /*decodedBytes=*/0, /*glyphs=*/0, /*kerningPairs=*/0,
             /*physicalResources=*/1,
             skin::SkinResourcePolicy::maximumScalableFontPaintBlendOperations +
                 1U),
         "Unrestricted adds no host scalable-font paint-work budget");
}

struct TemporaryDirectory {
  TemporaryDirectory() : root(std::filesystem::temp_directory_path() /
                              ("asobmashow-task13-" + std::to_string(++serial))) {
    std::filesystem::create_directories(root);
  }
  ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(root, error); }
  std::filesystem::path root;
  static inline std::atomic_uint64_t serial = 0;
};

struct FakeTextureDevice final : skin::SkinTextureDevice {
  bgfx::TextureHandle create(const image_decode::DecodedImageData &) override {
    ++creates;
    if (failAt != 0 && creates == failAt) return BGFX_INVALID_HANDLE;
    ++live;
    return {.idx = static_cast<std::uint16_t>(creates)};
  }
  void destroy(bgfx::TextureHandle handle) noexcept override {
    if (bgfx::isValid(handle)) { ++destroys; --live; }
  }
  bool update(bgfx::TextureHandle handle,
              const image_decode::DecodedImageData &image) override {
    if (!bgfx::isValid(handle) || image.rgba == nullptr) {
      return false;
    }
    ++updates;
    return true;
  }
  bool ownsCurrentThread() const noexcept override { return true; }
  int maximumTextureDimension() const noexcept override {
    return maximumDimension;
  }
  int creates = 0;
  int destroys = 0;
  int updates = 0;
  int live = 0;
  int failAt = 0;
  int maximumDimension = skin::SkinResourcePolicy::maximumDimension;
};

struct FakeMovieDevice final : skin::SkinMovieDevice {
  std::optional<skin::SkinMovieLoadResult>
  load(const std::filesystem::path &path,
       const skin::SkinMovieLoadLimits &limits, std::stop_token) override {
    ++loads;
    lastLimits = limits;
    loadedPaths.push_back(path);
    pathExistedDuringLoad = pathExistedDuringLoad &&
                            std::filesystem::is_regular_file(path);
    if (failAt != 0 && loads == failAt) {
      return std::nullopt;
    }
    const auto layout =
        skin::skinMovieDecodedLayout(resultWidth, resultHeight, limits);
    if (!layout) {
      return std::nullopt;
    }
    ++expensiveAllocations;
    const auto handle = skin::SkinMoviePlayerHandle{
        static_cast<std::uint64_t>(loads)};
    live.push_back(handle);
    if (stopAfterLoad != nullptr) {
      stopAfterLoad->request_stop();
    }
    return skin::SkinMovieLoadResult{
        .handle = handle,
        .width = resultWidth,
        .height = resultHeight,
        .durationMillis = 1'000,
        .decodedBytes = layout->residentBytes};
  }

  void destroy(skin::SkinMoviePlayerHandle handle) noexcept override {
    ++destroys;
    const auto found = std::ranges::find(live, handle);
    if (found != live.end()) {
      live.erase(found);
    }
  }

  bool ownsCurrentThread() const noexcept override { return true; }
  void beginFrame() noexcept override { ++begins; }
  skin::SkinMovieFramePreparationResult prepareFrame(
      skin::SkinMoviePlayerHandle, const skin::SkinMovieCommand &command,
      const skin::PlaySkinViewport &) override {
    ++prepares;
    preparedTimes.push_back(command.sourceTimeMillis);
    return {.ready = true, .drawable = true};
  }
  void discardFrame() noexcept override { ++discards; }
  void commitFrame() noexcept override { ++commits; }
  void submitPrepared(std::size_t index) noexcept override {
    submitted.push_back(index);
  }

  int loads = 0;
  int destroys = 0;
  int begins = 0;
  int prepares = 0;
  int discards = 0;
  int commits = 0;
  int failAt = 0;
  int expensiveAllocations = 0;
  int resultWidth = 80;
  int resultHeight = 40;
  skin::SkinMovieLoadLimits lastLimits;
  bool pathExistedDuringLoad = true;
  std::stop_source *stopAfterLoad = nullptr;
  std::vector<std::filesystem::path> loadedPaths;
  std::vector<skin::SkinMoviePlayerHandle> live;
  std::vector<std::int64_t> preparedTimes;
  std::vector<std::size_t> submitted;
};

skin::ValidatedBeatorajaSkinModel singleImageModel(std::string virtualPath) {
  skin::ValidatedBeatorajaSkinModel model;
  model.model.resources.emplace_back(skin::SkinImageResource{
      .id = 1, .authoredName = "image", .virtualPath = std::move(virtualPath)});
  model.model.objects.push_back(
      {.id = 1,
       .authoredName = "image-object",
       .payload = skin::SkinImageObject{.orderedStates = {{
           .resource = 1, .frames = {{.x = 0, .y = 0, .w = 40, .h = 20}}}}},
       .critical = true});
  return model;
}

skin::ValidatedBeatorajaSkinModel singleFontModel(std::string virtualPath,
                                                   bool critical,
                                                   std::string literal) {
  skin::ValidatedBeatorajaSkinModel model;
  skin::SkinFontResource font{
      .id = 1, .authoredName = "font", .virtualPath = std::move(virtualPath),
      .type = 0};
  if (font.virtualPath.ends_with(".fnt")) {
    font.bitmap = skin::SkinBitmapFontResource{
        .id = font.id, .virtualPath = font.virtualPath, .type = font.type};
  }
  model.model.resources.emplace_back(std::move(font));
  model.model.objects.push_back(
      {.id = 1,
       .authoredName = "font-object",
       .payload = skin::SkinTextObject{.font = 1,
                                       .literal = std::move(literal),
                                       .pointSize = 16},
       .critical = critical});
  return model;
}

void testChartBuiltinReaderOwnsBytesAndAccountingTransaction() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path source = temporary.root / "visible" / "ChartBuiltinFixture";
  fs::create_directories(source / "entry");
  std::ofstream(source / "entry/play.lr2skin") << "#INFORMATION,0,Play,test\n";
  const auto package =
      *skin::normalizePackageId("ChartBuiltinFixture").package;
  const auto entry =
      *skin::normalizeEntryPath(package, "entry/play.lr2skin").entry;
  skin::SkinStorageRoots roots{
      .visiblePackages = temporary.root / "visible",
      .privateRevisions = temporary.root / "revisions",
      .privateCatalog = temporary.root / "catalog",
      .profileOverlays = temporary.root / "overlays",
      .liveSources = true};
  auto aliases = skin::createPlatformSkinAliasDetector();
  skin::SkinTreeSnapshotter snapshotter(roots, *aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(),
         "chart built-in accounting fixture snapshots");
  if (!snapshot.prepared) return;
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease.has_value() && publishError.empty(),
         "chart built-in accounting fixture publishes");
  if (!lease) return;
  auto fileSystem = skin::LuaSkinFileSystem::create(
      {.revision = lease->readView(), .entry = entry, .storageRoots = roots});
  expect(fileSystem.fileSystem != nullptr,
         "chart built-in accounting fixture opens its package filesystem");
  if (!fileSystem.fileSystem) return;

  std::ifstream imageFile(
      fs::path(ASOBMASHOW_SOURCE_DIR) /
          "tests/fixtures/beatoraja_skin/resources/fixture.png",
      std::ios::binary);
  const std::vector<unsigned char> returnedBytes{
      std::istreambuf_iterator<char>(imageFile),
      std::istreambuf_iterator<char>()};
  const fs::path platformPath = temporary.root / "platform/stage.png";
  fs::create_directories(platformPath.parent_path());
  std::ofstream(platformPath, std::ios::binary).put('\0');

  skin::ValidatedBeatorajaSkinModel model;
  model.model.objects.push_back(
      {.id = 1,
       .authoredName = "stage-graph",
       .payload = skin::SkinGraphObject{.builtinImageReference = 100},
       .critical = false});
  skin::BeatorajaSkinConfiguration configuration;
  const skin::SkinBuiltinImageReader reader =
      [platformPath, returnedBytes](const fs::path &path,
                                    std::vector<unsigned char> &bytes,
                                    std::size_t maximumBytes,
                                    std::string *, std::stop_token) {
        if (path != platformPath || returnedBytes.size() > maximumBytes) {
          return false;
        }
        bytes = returnedBytes;
        return true;
      };
  struct ResetAccountingLimits {
    ~ResetAccountingLimits() {
      skin::resetSkinResourceAccountingLimitsForTesting();
    }
  } resetAccountingLimits;
  skin::SkinResourcePreparationService service;

  skin::setSkinResourceAccountingLimitsForTesting(
      returnedBytes.size(), std::numeric_limits<std::size_t>::max());
  auto exact = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *fileSystem.fileSystem,
       .model = model,
       .configuration = configuration,
       .builtinImagePaths = {{100, platformPath}},
       .builtinImageReader = reader});
  expect(exact.plan && exact.plan->builtinImageResources.contains(100) &&
             skin::skinResourceCommittedEncodedBytesForTesting() ==
                 returnedBytes.size(),
         "chart built-in charges the reader's retained bytes once rather "
         "than the stale one-byte path size");

  skin::setSkinResourceAccountingLimitsForTesting(
      returnedBytes.size() - 1U, std::numeric_limits<std::size_t>::max());
  auto oversized = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *fileSystem.fileSystem,
       .model = model,
       .configuration = configuration,
       .builtinImagePaths = {{100, platformPath}},
       .builtinImageReader = reader});
  const bool warned = std::ranges::any_of(
      oversized.diagnostics, [](const skin::SkinDiagnostic &diagnostic) {
        return diagnostic.code == "skin.resource.builtin_image_unavailable";
      });
  expect(oversized.plan &&
             !oversized.plan->builtinImageResources.contains(100) && warned &&
             skin::skinResourceCommittedEncodedBytesForTesting() == 0,
         "aggregate encoded-byte overage rolls back the optional chart image "
         "without charging rejected bytes");

  skin::setSkinResourceAccountingLimitsForTesting(
      returnedBytes.size(), std::numeric_limits<std::size_t>::max());
  std::stop_source stop;
  const skin::SkinBuiltinImageReader cancellingReader =
      [returnedBytes, &stop](const fs::path &,
                             std::vector<unsigned char> &bytes, std::size_t,
                             std::string *, std::stop_token) {
        bytes = returnedBytes;
        stop.request_stop();
        return true;
      };
  auto cancelled = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *fileSystem.fileSystem,
       .model = model,
       .configuration = configuration,
       .builtinImagePaths = {{100, platformPath}},
       .builtinImageReader = cancellingReader,
       .stop = stop.get_token()});
  expect(cancelled.cancelled && !cancelled.plan &&
             skin::skinResourceCommittedEncodedBytesForTesting() == 0,
         "cancellation after the bounded chart read publishes and charges "
         "nothing");
}

void testBitmapFontEncodedAccountingCommitsWithAtlasTransaction() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path source =
      temporary.root / "visible" / "BitmapAccountingFixture";
  const fs::path resources = source / "entry/resources";
  fs::create_directories(resources);
  std::ofstream(source / "entry/play.luaskin") << "return {}\n";

  const fs::path fixturePage =
      fs::path(ASOBMASHOW_SOURCE_DIR) /
      "tests/fixtures/beatoraja_skin/resources/bitmap-font/page.png";
  fs::copy_file(fixturePage, resources / "unique.png");
  fs::copy_file(fixturePage, resources / "shared.png");
  const std::string uniqueDescriptor =
      "info face=Unique size=10 padding=0,0,0,0\n"
      "common lineHeight=12 base=9 scaleW=40 scaleH=20 pages=1\n"
      "page id=0 file=unique.png\n"
      "chars count=1\n"
      "char id=65 x=9 y=0 width=6 height=8 xoffset=1 yoffset=1 "
      "xadvance=7 page=0 chnl=15\n";
  const std::string sharedDescriptor =
      "info face=Shared size=10 padding=0,0,0,0\n"
      "common lineHeight=12 base=9 scaleW=40 scaleH=20 pages=1\n"
      "page id=0 file=shared.png\n"
      "chars count=1\n"
      "char id=65 x=9 y=0 width=6 height=8 xoffset=1 yoffset=1 "
      "xadvance=7 page=0 chnl=15\n";
  std::ofstream(resources / "unique.fnt", std::ios::binary)
      << uniqueDescriptor;
  std::ofstream(resources / "shared.fnt", std::ios::binary)
      << sharedDescriptor;

  const auto package =
      *skin::normalizePackageId("BitmapAccountingFixture").package;
  const auto entry =
      *skin::normalizeEntryPath(package, "entry/play.luaskin").entry;
  skin::SkinStorageRoots roots{
      .visiblePackages = temporary.root / "visible",
      .privateRevisions = temporary.root / "revisions",
      .privateCatalog = temporary.root / "catalog",
      .profileOverlays = temporary.root / "overlays",
      .liveSources = true};
  auto aliases = skin::createPlatformSkinAliasDetector();
  skin::SkinTreeSnapshotter snapshotter(roots, *aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(),
         "bitmap accounting fixture creates a live revision");
  if (!snapshot.prepared) {
    return;
  }
  auto stagedFs = skin::LuaSkinFileSystem::create(
      {.revision = snapshot.prepared->readView(),
       .entry = entry,
       .storageRoots = roots});
  expect(stagedFs.fileSystem != nullptr,
         "bitmap accounting fixture creates an entry-aware filesystem");
  if (!stagedFs.fileSystem) {
    return;
  }

  skin::ValidatedBeatorajaSkinModel model;
  model.model.resources.emplace_back(skin::SkinFontResource{
      .id = 1,
      .authoredName = "optional-too-large-atlas",
      .virtualPath = "resources/unique.fnt",
      .type = 0,
      .fallbacks = {{.virtualPath = "resources/shared.fnt", .type = 0}},
      .bitmap = skin::SkinBitmapFontResource{
          .id = 1, .virtualPath = "resources/unique.fnt", .type = 0}});
  model.model.resources.emplace_back(skin::SkinFontResource{
      .id = 2,
      .authoredName = "accepted-shared-atlas",
      .virtualPath = "resources/shared.fnt",
      .type = 0,
      .bitmap = skin::SkinBitmapFontResource{
          .id = 2, .virtualPath = "resources/shared.fnt", .type = 0}});
  model.model.objects.push_back(
      {.id = 1,
       .authoredName = "optional-text",
       .payload = skin::SkinTextObject{
           .font = 1, .literal = "A", .pointSize = 16},
       .critical = false});
  model.model.objects.push_back(
      {.id = 2,
       .authoredName = "accepted-text",
       .payload = skin::SkinTextObject{
           .font = 2, .literal = "A", .pointSize = 16},
       .critical = true});

  const std::size_t encodedPageBytes = fs::file_size(fixturePage);
  const std::size_t firstRequestEncodedBytes =
      uniqueDescriptor.size() + sharedDescriptor.size() +
      encodedPageBytes * 2U;
  const std::size_t committedSharedEncodedBytes =
      sharedDescriptor.size() + encodedPageBytes;
  skin::setSkinResourceAccountingLimitsForTesting(
      firstRequestEncodedBytes, /*maximumAtlasSessionBytes=*/3'200);
  struct ResetAccountingLimits {
    ~ResetAccountingLimits() {
      skin::resetSkinResourceAccountingLimitsForTesting();
    }
  } resetAccountingLimits;

  const auto hasAtlasLimit = [](const auto &diagnostics) {
    return std::ranges::any_of(diagnostics, [](const auto &diagnostic) {
      return diagnostic.code == "skin.resource.atlas_limit";
    });
  };
  skin::BeatorajaSkinConfiguration configuration;
  skin::SkinResourcePreparationService service;
  const auto validation = service.validateResources(
      {.revision = snapshot.prepared->readView(),
       .entry = entry,
       .fileSystem = *stagedFs.fileSystem,
       .model = model,
       .configuration = configuration});
  expect(validation.valid && hasAtlasLimit(validation.diagnostics) &&
             skin::skinResourceCommittedEncodedBytesForTesting() ==
                 committedSharedEncodedBytes,
         "validation discards optional atlas accounting identities and "
         "charges the later accepted descriptor/page exactly once");

  stagedFs.fileSystem.reset();
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease && publishError.empty(),
         "bitmap accounting fixture publishes after validation");
  if (!lease) {
    return;
  }
  auto leasedFs = skin::LuaSkinFileSystem::create(
      {.revision = lease->readView(), .entry = entry, .storageRoots = roots});
  expect(leasedFs.fileSystem != nullptr,
         "published bitmap accounting filesystem is available");
  if (!leasedFs.fileSystem) {
    return;
  }
  auto planned = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = model,
       .configuration = configuration});
  expect(planned.plan && hasAtlasLimit(planned.diagnostics) &&
             planned.plan->atlases.size() == 1 &&
             !planned.plan->textAtlasesByObject.contains(1) &&
             planned.plan->textAtlasesByObject.contains(2) &&
             skin::skinResourceCommittedEncodedBytesForTesting() ==
                 committedSharedEncodedBytes,
         "planning publishes only the later aliased atlas and commits its "
         "descriptor/page encoded bytes exactly once");
}

void testSecurePreparationLeaseAliasAndCatalogLifetime() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path source = temporary.root / "visible" / "Task13Fixture";
  fs::create_directories(source / "entry/resources");
  std::ofstream(source / "entry/play.luaskin") << "return {}\n";
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/fixture.png",
                source / "entry/resources/fixture.png");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/fixture.jpg",
                source / "entry/resources/fixture.jpg");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/fixture.ttf",
                source / "entry/resources/fixture.ttf");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/fixture.png",
                source / "entry/resources/movie.MP4");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/fixture.jpg",
                source / "entry/resources/second.webm");
  for (std::size_t index = 0;
       index <= skin::SkinResourcePolicy::maximumMovieDecoders; ++index) {
    fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                      "tests/fixtures/beatoraja_skin/resources/fixture.png",
                  source / "entry/resources" /
                      ("movie-cap-" + std::to_string(index) + ".MP4"));
  }
  const auto writePpm = [](const fs::path &path, int width) {
    std::ofstream stream(path, std::ios::binary);
    stream << "P6\n" << width << " 1\n255\n";
    for (int pixel = 0; pixel < width; ++pixel) {
      stream.put(static_cast<char>(pixel == 0 ? 255 : 0));
      stream.put(static_cast<char>(pixel == 1 ? 255 : 0));
      stream.put(0);
    }
  };
  writePpm(source / "entry/resources/image-default.ppm", 1);
  writePpm(source / "entry/resources/image-selected.ppm", 2);
  for (std::size_t index = 0;
       index <= skin::SkinResourcePolicy::maximumResources;
       ++index) {
    writePpm((source / "entry/resources") /
                 ("cap-" + std::to_string(index) + ".ppm"),
             1);
  }
  for (const std::string_view name : {
           "font-primary-selected.ttf", "font-fallback-selected.ttf",
           "font-secondary-selected.otf"}) {
    fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                      "tests/fixtures/beatoraja_skin/resources/fixture.ttf",
                  source / "entry/resources" / name);
  }
  fs::create_directories(source / "entry/resources/bitmap-font");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/bitmap-font/fixture.fnt",
                source / "entry/resources/bitmap-font/fixture.fnt");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/bitmap-font/page.png",
                source / "entry/resources/bitmap-font/page.png");
  std::ofstream(source / "entry/resources/bitmap-font/missing-page.fnt")
      << "info face=Missing size=10 padding=0,0,0,0\n"
         "common lineHeight=12 base=9 scaleW=40 scaleH=20 pages=1\n"
         "page id=0 file=absent.png\n"
         "chars count=1\n"
         "char id=65 x=0 y=0 width=6 height=8 xoffset=1 yoffset=1 "
         "xadvance=7 page=0 chnl=15\n";
  std::ofstream(source / "entry/resources/bitmap-font/primary.fnt")
      << "info face=Primary size=10 padding=0,0,0,0\n"
         "common lineHeight=12 base=9 scaleW=40 scaleH=20 pages=1\n"
         "page id=0 file=page.png\n"
         "chars count=1\n"
         "char id=65 x=9 y=0 width=6 height=8 xoffset=1 yoffset=1 "
         "xadvance=7 page=0 chnl=15\n";
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) / "assets/fonts/fa-solid-900.ttf",
                source / "entry/resources/icons.ttf");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) / "bgfx/bgfx/examples/runtime/font/signika-regular.ttf",
                source / "entry/resources/signika.ttf");
  const auto package = *skin::normalizePackageId("Task13Fixture").package;
  const auto entry = *skin::normalizeEntryPath(package, "entry/play.luaskin").entry;
  skin::SkinStorageRoots roots{.visiblePackages=temporary.root/"visible",
                               .privateRevisions=temporary.root/"revisions",
                               .privateCatalog=temporary.root/"catalog",
                               .profileOverlays=temporary.root/"overlays",
                               .liveSources=true};
  auto aliases = skin::createPlatformSkinAliasDetector();
  skin::SkinTreeSnapshotter snapshotter(roots, *aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(), "resource fixture creates a live revision");
  if (!snapshot.prepared) return;
  auto stagedFs = skin::LuaSkinFileSystem::create({.revision=snapshot.prepared->readView(), .entry=entry, .storageRoots=roots});
  expect(stagedFs.fileSystem != nullptr, "staged resource filesystem is entry-aware");
  if (!stagedFs.fileSystem) return;
  skin::ValidatedBeatorajaSkinModel model;
  model.model.resources.emplace_back(skin::SkinImageResource{.id=1, .authoredName="one", .virtualPath="resources/fixture.png"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=2, .authoredName="two", .virtualPath="resources/fixture.png"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=3, .authoredName="three", .virtualPath="resources/fixture.jpg"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=4, .authoredName="unused", .virtualPath="resources/does-not-exist.png"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=5, .authoredName="optional", .virtualPath="resources/does-not-exist.png"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=6, .authoredName="disabled", .virtualPath="resources/does-not-exist.png"});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=7, .authoredName="fixture-font", .virtualPath="resources/fixture.ttf", .type=0});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=8, .authoredName="fallback-font", .virtualPath="resources/icons.ttf", .type=0, .fallbacks={{.virtualPath="resources/fixture.ttf", .type=0}}});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=9, .authoredName="kerning-font", .virtualPath="resources/signika.ttf", .type=0});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=10, .authoredName="bitmap-font", .virtualPath="resources/bitmap-font/fixture.fnt", .type=0, .bitmap=skin::SkinBitmapFontResource{.id=10, .virtualPath="resources/bitmap-font/fixture.fnt", .type=0}});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=11, .authoredName="distance-font", .virtualPath="resources/bitmap-font/fixture.fnt", .type=1, .bitmap=skin::SkinBitmapFontResource{.id=11, .virtualPath="resources/bitmap-font/fixture.fnt", .type=1}});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=12, .authoredName="colored-distance-font", .virtualPath="resources/bitmap-font/fixture.fnt", .type=2, .bitmap=skin::SkinBitmapFontResource{.id=12, .virtualPath="resources/bitmap-font/fixture.fnt", .type=2}});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=13, .authoredName="bitmap-fallback-font", .virtualPath="resources/bitmap-font/primary.fnt", .type=0, .fallbacks={{.virtualPath="resources/bitmap-font/fixture.fnt", .type=0}}, .bitmap=skin::SkinBitmapFontResource{.id=13, .virtualPath="resources/bitmap-font/primary.fnt", .type=0}});
  model.model.objects.push_back({.id=1, .authoredName="primary", .payload=skin::SkinImageObject{.orderedStates={{.resource=1, .frames={{.x=0,.y=0,.w=40,.h=20,.gridColumns=4,.gridRows=2}}}}}, .critical=true});
  model.model.objects.push_back({.id=2, .authoredName="alias", .payload=skin::SkinImageObject{.orderedStates={{.resource=2, .frames={{.x=2,.y=3,.w=20,.h=10,.gridColumns=2,.gridRows=1}}}}}, .critical=true});
  model.model.objects.push_back({.id=3, .authoredName="jpeg", .payload=skin::SkinImageObject{.orderedStates={{.resource=3, .frames={{.x=0,.y=0,.w=40,.h=20}}}}}, .critical=true});
  model.model.objects.push_back({.id=4, .authoredName="optional", .payload=skin::SkinImageObject{.orderedStates={{.resource=5, .frames={{.x=0,.y=0,.w=40,.h=20}}}}}, .critical=false});
  model.model.objects.push_back({.id=5, .authoredName="disabled", .payload=skin::SkinImageObject{.orderedStates={{.resource=6, .frames={{.x=0,.y=0,.w=40,.h=20}}}}}, .critical=false});
  model.model.objects.push_back({.id=6, .authoredName="caption", .payload=skin::SkinTextObject{.font=7, .value=skin::SkinStringPropertyId{.value=1}, .literal="AV 123 日本", .pointSize=16}, .critical=true});
  model.model.objects.push_back({.id=7, .authoredName="styled-caption", .payload=skin::SkinTextObject{.font=7, .literal="AV", .pointSize=24, .outlineRgba={255,0,0,255}, .outlineWidth=1.0, .shadowRgba={0,0,255,128}, .shadowOffsetX=1.0, .shadowOffsetY=2.0, .shadowSmoothness=0.5}, .critical=true});
  model.model.objects.push_back({.id=8, .authoredName="fallback-caption", .payload=skin::SkinTextObject{.font=8, .literal="日本", .pointSize=16}, .critical=true});
  model.model.objects.push_back({.id=9, .authoredName="kerning-caption", .payload=skin::SkinTextObject{.font=9, .literal="AV", .pointSize=16}, .critical=true});
  model.model.objects.push_back({.id=10, .authoredName="bitmap-caption", .payload=skin::SkinTextObject{.font=10, .literal="AV\xF0\x9F\x99\x82", .pointSize=20}, .critical=true});
  model.model.objects.push_back({.id=11, .authoredName="distance-caption", .payload=skin::SkinTextObject{.font=11, .literal="A", .pointSize=20, .outlineRgba={1,2,3,4}, .outlineWidth=0.5, .shadowRgba={5,6,7,8}, .shadowOffsetX=4.0, .shadowOffsetY=2.0, .shadowSmoothness=0.4}, .critical=true});
  model.model.objects.push_back({.id=12, .authoredName="colored-distance-caption", .payload=skin::SkinTextObject{.font=12, .literal="A", .pointSize=20}, .critical=true});
  model.model.objects.push_back({.id=13, .authoredName="bitmap-fallback-caption", .payload=skin::SkinTextObject{.font=13, .literal="\xE3\x80\x9C", .pointSize=20}, .critical=true});
  model.model.objects.push_back({.id=14, .authoredName="bitmap-style-caption", .payload=skin::SkinTextObject{.font=10, .literal="V", .pointSize=20, .outlineRgba={9,8,7,6}, .outlineWidth=2.0, .shadowRgba={5,4,3,2}, .shadowOffsetX=3.0, .shadowOffsetY=4.0, .shadowSmoothness=0.75}, .critical=true});
  model.disabledOptionalObjects.push_back(5);
  skin::BeatorajaSkinConfiguration configuration;
  const std::array<std::string, 1> runtimeStrings{"Artist 日本 42"};
  skin::SkinResourcePreparationService service;
  std::ofstream(source / "entry/play.luaskin", std::ios::app) << "-- different revision\n";
  auto mismatchSnapshot = snapshotter.snapshot(source, package, {}, {});
  expect(mismatchSnapshot.prepared.has_value(),
         "second live source revision is available for provenance testing");
  if (mismatchSnapshot.prepared) {
    auto mismatchFs = skin::LuaSkinFileSystem::create({.revision=mismatchSnapshot.prepared->readView(), .entry=entry, .storageRoots=roots});
    expect(mismatchFs.fileSystem != nullptr, "second live revision creates an independent filesystem");
    if (!mismatchFs.fileSystem) return;
    const auto mismatch = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*mismatchFs.fileSystem, .model=model, .configuration=configuration});
    expect(!mismatch.valid && !mismatch.diagnostics.empty() &&
               mismatch.diagnostics.front().code == "skin.resource.path_invalid",
           "preparation rejects a filesystem from another live source revision before reading bytes");
  }
  const auto hasDiagnostic = [](const std::vector<skin::SkinDiagnostic> &diagnostics,
                                std::string_view code) {
    for (const auto &item : diagnostics) if (item.code == code) return true;
    return false;
  };
  const auto criticalMissingModel =
      singleImageModel("resources/does-not-exist.png");
  const auto criticalMissing = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=criticalMissingModel, .configuration=configuration});
  expect(!criticalMissing.valid && hasDiagnostic(criticalMissing.diagnostics, "skin.resource.missing_critical"),
         "a live critical resource missing from the selected source revision is a blocking error");
  auto invalidCropModel = singleImageModel("resources/fixture.png");
  auto &badFrames = std::get<skin::SkinImageObject>(invalidCropModel.model.objects[0].payload).orderedStates.front().frames;
  badFrames.front().x = 39;
  badFrames.front().w = 2;
  const auto invalidCrop = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=invalidCropModel, .configuration=configuration});
  expect(invalidCrop.valid && !hasDiagnostic(invalidCrop.diagnostics, "skin.resource.sprite_bounds"),
         "Beatoraja-compatible source crops outside an image remain selectable");
  const auto bitmapFontModel = singleFontModel(
      "resources/bitmap-font/fixture.fnt", true, "AV\xF0\x9F\x99\x82");
  const auto bitmapFont = service.validateResources(
      {.revision=snapshot.prepared->readView(), .entry=entry,
       .fileSystem=*stagedFs.fileSystem, .model=bitmapFontModel,
       .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(bitmapFont.valid &&
             !hasDiagnostic(bitmapFont.diagnostics,
                            "skin.resource.font_format_unsupported"),
         "a text BMFont descriptor with two pages and a supplementary glyph validates");
  const auto optionalMissingPageModel = singleFontModel(
      "resources/bitmap-font/missing-page.fnt", false, "A");
  const auto optionalMissingPage = service.validateResources(
      {.revision=snapshot.prepared->readView(), .entry=entry,
       .fileSystem=*stagedFs.fileSystem, .model=optionalMissingPageModel,
       .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(optionalMissingPage.valid &&
             hasDiagnostic(optionalMissingPage.diagnostics,
                           "skin.resource.font_missing"),
         "an optional bitmap-font page failure is a non-blocking diagnostic");
  skin::ValidatedBeatorajaSkinModel missingFallbackModel;
  missingFallbackModel.model.resources.emplace_back(skin::SkinFontResource{
      .id = 1,
      .authoredName = "primary-with-missing-fallback",
      .virtualPath = "resources/bitmap-font/primary.fnt",
      .type = 0,
      .fallbacks = {{.virtualPath =
                         "resources/bitmap-font/missing-page.fnt",
                     .type = 0}},
      .bitmap = skin::SkinBitmapFontResource{
          .id = 1,
          .virtualPath = "resources/bitmap-font/primary.fnt",
          .type = 0}});
  missingFallbackModel.model.objects.push_back(
      {.id = 1,
       .authoredName = "primary-text",
       .payload = skin::SkinTextObject{
           .font = 1, .literal = "A", .pointSize = 20},
       .critical = true});
  const auto missingFallback = service.validateResources(
      {.revision = snapshot.prepared->readView(),
       .entry = entry,
       .fileSystem = *stagedFs.fileSystem,
       .model = missingFallbackModel,
       .configuration = configuration});
  expect(missingFallback.valid &&
             hasDiagnostic(missingFallback.diagnostics,
                           "skin.resource.font_missing"),
         "an unusable bitmap fallback face is skipped without discarding its "
         "valid critical primary face");
  const auto unknownGlyphModel =
      singleFontModel("resources/fixture.ttf", true, "\xF4\x8F\xBF\xBF");
  const auto unknownGlyph = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=unknownGlyphModel, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(!unknownGlyph.valid && hasDiagnostic(unknownGlyph.diagnostics, "skin.resource.glyph_missing"),
         "unknown live text glyphs fail synchronously before resource publication");
  const auto compatibleUnknownGlyph = service.validateResources(
      {.revision = snapshot.prepared->readView(),
       .entry = entry,
       .fileSystem = *stagedFs.fileSystem,
       .model = unknownGlyphModel,
       .configuration = configuration,
       .requiredRuntimeStrings = runtimeStrings,
       .safetyPolicy =
           skin::SkinSafetyPolicy(skin::SkinSafetyLevel::Unrestricted)});
  expect(compatibleUnknownGlyph.valid &&
             !hasDiagnostic(compatibleUnknownGlyph.diagnostics,
                            "skin.resource.glyph_missing"),
         "type-5 compatibility synthesizes Beatoraja's missing-glyph box "
         "instead of rejecting the atlas");
  std::string expensiveText;
  for (char value = '!'; value <= '~'; ++value) expensiveText.push_back(value);
  skin::ValidatedBeatorajaSkinModel rejectedAtlasModel;
  rejectedAtlasModel.model.resources.emplace_back(skin::SkinFontResource{
      .id = 100,
      .authoredName = "rejected-outline-font",
      .virtualPath = "resources/signika.ttf",
      .type = 0});
  rejectedAtlasModel.model.objects.push_back(
      {.id = 100,
       .authoredName = "first-rejected-outline",
       .payload = skin::SkinTextObject{
           .font = 100,
           .literal = expensiveText,
           .pointSize = 160,
           .outlineRgba = {255, 0, 0, 255},
           .outlineWidth = 8.0},
       .critical = false});
  rejectedAtlasModel.model.objects.push_back(
      {.id = 101,
       .authoredName = "second-rejected-outline",
       .payload = skin::SkinTextObject{
           .font = 100,
           .literal = expensiveText,
           .pointSize = 160,
           .outlineRgba = {0, 255, 0, 255},
           .outlineWidth = 8.0},
       .critical = false});
  skin::setSkinResourceAccountingLimitsForTesting(
      std::numeric_limits<std::size_t>::max(),
      /*maximumAtlasSessionBytes=*/0);
  skin::resetSkinTextAtlasPaintBlendOperationsForTesting();
  const auto rejectedAtlases = service.validateResources(
      {.revision = snapshot.prepared->readView(),
       .entry = entry,
       .fileSystem = *stagedFs.fileSystem,
       .model = rejectedAtlasModel,
       .configuration = configuration});
  const std::size_t rejectedAtlasPaint =
      skin::skinTextAtlasPaintBlendOperationsForTesting();
  skin::resetSkinResourceAccountingLimitsForTesting();
  expect(rejectedAtlases.valid &&
             hasDiagnostic(rejectedAtlases.diagnostics,
                           "skin.resource.atlas_limit") &&
             rejectedAtlasPaint >
                 skin::SkinResourcePolicy::
                         maximumScalableFontPaintBlendOperations /
                     2U &&
             rejectedAtlasPaint <=
                 skin::SkinResourcePolicy::
                     maximumScalableFontPaintBlendOperations,
         "repeated optional post-build accounting rejection cannot repaint "
         "the session scalable-font work budget");
  const auto validated = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=model, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(validated.valid && !validated.cancelled && validated.diagnostics.size() == 1 &&
             validated.diagnostics.front().code == "skin.resource.missing_optional",
         "optional missing resources warn while unreferenced declarations do not become false critical failures");
  stagedFs.fileSystem.reset();
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease && publishError.empty(), "live revision publishes after non-retaining validation");
  if (!lease) return;
  const auto weak = lease->weakPin();
  auto leasedFs = skin::LuaSkinFileSystem::create({.revision=lease->readView(), .entry=entry, .storageRoots=roots});
  expect(leasedFs.fileSystem != nullptr, "published resource filesystem is available");
  if (!leasedFs.fileSystem) return;
  skin::ValidatedBeatorajaSkinModel practiceModel;
  practiceModel.model.objects.push_back(
      {.id = 15,
       .authoredName = "practice",
       .payload = skin::SkinPracticeObject{.visibleItems = 0},
       .critical = false});
  skin::resetSkinResourcePlatformAssetReadsForTesting();
  auto practicePlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = practiceModel,
       .configuration = configuration});
  expect(practicePlan.plan && practicePlan.plan->atlases.size() == 1 &&
             practicePlan.plan->textAtlasesByObject.size() == 1 &&
             practicePlan.plan->textAtlasesByObject.contains(15) &&
             practicePlan.plan->atlases.front().key.pointSize == 18 &&
             practicePlan.plan->atlases.front().glyphs.contains(U'A') &&
             practicePlan.plan->atlases.front().glyphs.contains(U'0') &&
             practicePlan.plan->atlases.front().glyphs.contains(U'#') &&
             skin::skinResourcePlatformAssetReadsForTesting() == 1,
         "legacy SkinPractice prepares one fixed system-font atlas with its "
         "complete source text domain through the platform asset bridge");
  practicePlan.plan.reset();

  skin::ValidatedBeatorajaSkinModel positivePracticeModel;
  positivePracticeModel.model.objects.push_back(
      {.id = 16,
       .authoredName = "positive-practice",
       .payload = skin::SkinPracticeObject{.visibleItems = 10},
       .critical = false});
  auto positivePracticePlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = positivePracticeModel,
       .configuration = configuration,
       .practiceMode = true});
  expect(positivePracticePlan.plan &&
             positivePracticePlan.plan->atlases.empty() &&
             positivePracticePlan.plan->textAtlasesByObject.empty() &&
             skin::skinResourcePlatformAssetReadsForTesting() == 1,
         "positive-visible-item SkinPractice uses the authored menu and does "
         "not allocate the legacy fallback font");

  skin::ValidatedBeatorajaSkinModel bgaOnlyModel;
  bgaOnlyModel.model.objects.push_back(
      {.id = 17,
       .authoredName = "bga",
       .payload = skin::SkinBgaObject{},
       .critical = false});
  auto normalBgaPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = bgaOnlyModel,
       .configuration = configuration,
       .practiceMode = false});
  expect(normalBgaPlan.plan && normalBgaPlan.plan->atlases.empty() &&
             normalBgaPlan.plan->textAtlasesByObject.empty() &&
             skin::skinResourcePlatformAssetReadsForTesting() == 1,
         "an ordinary-play BGA does not eagerly allocate the Practice "
         "fallback font");

  auto practiceBgaPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = bgaOnlyModel,
       .configuration = configuration,
       .practiceMode = true});
  expect(practiceBgaPlan.plan && practiceBgaPlan.plan->atlases.size() == 1 &&
             practiceBgaPlan.plan->textAtlasesByObject.contains(17) &&
             skin::skinResourcePlatformAssetReadsForTesting() == 2,
         "a BGA with no Practice object prepares the legacy fallback font "
         "only for a fixed Practice-mode session");

  positivePracticeModel.model.objects.push_back(
      {.id = 18,
       .authoredName = "practice-bga",
       .payload = skin::SkinBgaObject{},
       .critical = false});
  auto authoredPracticeBgaPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = positivePracticeModel,
       .configuration = configuration,
       .practiceMode = true});
  expect(authoredPracticeBgaPlan.plan &&
             authoredPracticeBgaPlan.plan->atlases.empty() &&
             authoredPracticeBgaPlan.plan->textAtlasesByObject.empty() &&
             skin::skinResourcePlatformAssetReadsForTesting() == 2,
         "an authored positive-item Practice object suppresses the BGA "
         "legacy fallback plan");
  positivePracticePlan.plan.reset();
  normalBgaPlan.plan.reset();
  practiceBgaPlan.plan.reset();
  authoredPracticeBgaPlan.plan.reset();

  expect(skin::skinResourcePathIsMovie("skin/source.MP4") &&
             skin::skinResourcePathIsMovie("skin/source.m4v") &&
             skin::skinResourcePathIsMovie("skin/source.WMV") &&
             skin::skinResourcePathIsMovie("skin/source.webm") &&
             skin::skinResourcePathIsMovie("skin/source.mpg") &&
             skin::skinResourcePathIsMovie("skin/source.MPEG") &&
             skin::skinResourcePathIsMovie("skin/source.m1v") &&
             skin::skinResourcePathIsMovie("skin/source.M2V") &&
             skin::skinResourcePathIsMovie("skin/source.avi") &&
             !skin::skinResourcePathIsMovie("skin/source.png") &&
             !skin::skinResourcePathIsMovie("skin/source.avi.png"),
         "movie classification exactly matches pinned VideoFormat extensions case-insensitively");

  skin::ValidatedBeatorajaSkinModel movieModel;
  movieModel.model.resources.emplace_back(skin::SkinMovieResource{
      .id = 21, .virtualPath = "resources/movie.MP4", .authoredOrdinal = 1});
  movieModel.model.resources.emplace_back(skin::SkinMovieResource{
      .id = 22, .virtualPath = "resources/movie.MP4", .authoredOrdinal = 2});
  movieModel.model.objects.push_back(
      {.id = 21,
       .authoredName = "movie-one",
       .payload = skin::SkinImageObject{.orderedStates = {{.resource = 21}}},
       .critical = true});
  movieModel.model.objects.push_back(
      {.id = 22,
       .authoredName = "movie-two",
       .payload = skin::SkinImageObject{.orderedStates = {{.resource = 22}}},
       .critical = true});
  auto movieDevice = std::make_shared<FakeMovieDevice>();
  auto movies = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = movieModel,
       .configuration = configuration,
       .device = movieDevice});
  expect(movies.catalog && !movies.cancelled && movies.diagnostics.empty() &&
             movieDevice->loads == 1 && movieDevice->live.size() == 1 &&
             movieDevice->pathExistedDuringLoad &&
             movies.catalog->findMovie(21) && movies.catalog->findMovie(22) &&
             movies.catalog->findMovie(21)->handle ==
                 movies.catalog->findMovie(22)->handle,
         "deduplicated movie paths materialize and load exactly once while retaining typed aliases");

  auto compatibilityWithoutMovieDevice = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = movieModel,
       .configuration = configuration,
       .device = nullptr,
       .safetyPolicy =
           skin::SkinSafetyPolicy{skin::SkinSafetyLevel::Unrestricted}});
  expect(compatibilityWithoutMovieDevice.catalog &&
             compatibilityWithoutMovieDevice.catalog->movieCount() == 0 &&
             hasDiagnostic(compatibilityWithoutMovieDevice.diagnostics,
                           "skin.movie.device_unavailable") &&
             std::ranges::none_of(
                 compatibilityWithoutMovieDevice.diagnostics,
                 [](const skin::SkinDiagnostic &diagnostic) {
                   return diagnostic.severity ==
                          skin::DiagnosticSeverity::Error;
                 }),
         "Beatoraja-compatibility loading omits a movie when no device is "
         "available instead of rejecting the skin");

  auto compatibilityFailingMovieDevice = std::make_shared<FakeMovieDevice>();
  compatibilityFailingMovieDevice->resultWidth = 0;
  auto compatibilityFailedMovie = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = movieModel,
       .configuration = configuration,
       .device = compatibilityFailingMovieDevice,
       .safetyPolicy =
           skin::SkinSafetyPolicy{skin::SkinSafetyLevel::Unrestricted}});
  expect(compatibilityFailedMovie.catalog &&
             compatibilityFailedMovie.catalog->movieCount() == 0 &&
             hasDiagnostic(compatibilityFailedMovie.diagnostics,
                           "skin.movie.load_failed") &&
             compatibilityFailingMovieDevice->live.empty() &&
             std::ranges::none_of(
                 compatibilityFailedMovie.diagnostics,
                 [](const skin::SkinDiagnostic &diagnostic) {
                   return diagnostic.severity ==
                          skin::DiagnosticSeverity::Error;
                 }),
         "Beatoraja-compatibility loading omits an unopenable movie object "
         "without rejecting the rest of the skin");
  const auto defaultMovieLayout = skin::skinMovieDecodedLayout(
      80, 40,
      {.maximumDimension = skin::SkinResourcePolicy::maximumDimension,
       .maximumRgbaBytes = skin::SkinResourcePolicy::maximumImageBytes,
       .maximumDecodedBytes =
           skin::SkinResourcePolicy::maximumSessionDecodedBytes});
  expect(defaultMovieLayout && movies.catalog->decodedBytes() ==
                                   defaultMovieLayout->residentBytes &&
             movieDevice->lastLimits.maximumDimension ==
                 skin::SkinResourcePolicy::maximumDimension,
         "movie preparation publishes its bounded live decoded working set");
  skin::SkinMovieCommand movieCommand{.resource = 21,
                                      .sourceTimeMillis = 375};
  const std::array<const skin::SkinMovieCommand *, 1> movieCommands{
      &movieCommand};
  const skin::PlaySkinViewport movieViewport{.valid = true};
  const auto preparedMovieFrame =
      movies.catalog->prepareFrame(movieCommands, movieViewport);
  movies.catalog->commitFrame();
  movies.catalog->submitPrepared(0);
  movies.catalog->discardFrame();
  expect(preparedMovieFrame.ready && movieDevice->loads == 1 &&
             movieDevice->begins == 1 && movieDevice->prepares == 1 &&
             movieDevice->preparedTimes == std::vector<std::int64_t>{375} &&
             movieDevice->commits == 1 &&
             movieDevice->submitted == std::vector<std::size_t>{0},
         "frame preparation seeks and submits an already-owned player without loading or opening another source");
  const auto materializedMovie = movieDevice->loadedPaths.front();
  movies.catalog.reset();
  expect(movieDevice->destroys == 1 && movieDevice->live.empty() &&
             !fs::exists(materializedMovie),
         "movie catalog teardown destroys the shared player and materialized source exactly once");

  skin::ValidatedBeatorajaSkinModel decoderCapModel;
  for (std::size_t index = 0;
       index <= skin::SkinResourcePolicy::maximumMovieDecoders; ++index) {
    const skin::SkinResourceId id = static_cast<skin::SkinResourceId>(100 + index);
    decoderCapModel.model.resources.emplace_back(skin::SkinMovieResource{
        .id = id,
        .virtualPath = "resources/movie-cap-" + std::to_string(index) +
                       ".MP4",
        .authoredOrdinal = static_cast<std::uint32_t>(index + 1)});
    decoderCapModel.model.objects.push_back(
        {.id = static_cast<skin::SkinObjectId>(id),
         .authoredName = "movie-cap-" + std::to_string(index),
         .payload = skin::SkinImageObject{.orderedStates = {{.resource = id}}},
         .critical = true});
  }
  auto cappedMovieDevice = std::make_shared<FakeMovieDevice>();
  auto cappedMovies = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = decoderCapModel,
       .configuration = configuration,
       .device = cappedMovieDevice});
  expect(!cappedMovies.catalog && !cappedMovies.cancelled &&
             hasDiagnostic(cappedMovies.diagnostics,
                           "skin.movie.session_limit") &&
             cappedMovieDevice->loads == 0 &&
             cappedMovieDevice->expensiveAllocations == 0 &&
             cappedMovieDevice->live.empty(),
         "unique referenced movies above the decoder cap fail before any player starts");

  auto oversizedMovieDevice = std::make_shared<FakeMovieDevice>();
  oversizedMovieDevice->resultWidth =
      skin::SkinResourcePolicy::maximumDimension + 1;
  auto oversizedMovie = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = movieModel,
       .configuration = configuration,
       .device = oversizedMovieDevice});
  expect(!oversizedMovie.catalog && oversizedMovieDevice->loads == 1 &&
             oversizedMovieDevice->expensiveAllocations == 0 &&
             oversizedMovieDevice->live.empty(),
         "oversized codec metadata is rejected inside the load contract "
         "before costly allocation or decoder-thread start");

  auto oversizedDecodedMovieDevice = std::make_shared<FakeMovieDevice>();
  oversizedDecodedMovieDevice->resultWidth =
      skin::SkinResourcePolicy::maximumDimension;
  oversizedDecodedMovieDevice->resultHeight =
      skin::SkinResourcePolicy::maximumDimension;
  auto oversizedDecodedMovie = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = movieModel,
       .configuration = configuration,
       .device = oversizedDecodedMovieDevice});
  expect(!oversizedDecodedMovie.catalog &&
             oversizedDecodedMovieDevice->loads == 1 &&
             oversizedDecodedMovieDevice->expensiveAllocations == 0 &&
             oversizedDecodedMovieDevice->live.empty(),
         "in-range dimensions whose RGBA frame exceeds the per-image decoded "
         "budget are rejected before costly allocation");

  auto sharedBudgetMovieDevice = std::make_shared<FakeMovieDevice>();
  const std::size_t sharedInitialBytes =
      skin::SkinResourcePolicy::maximumSessionDecodedBytes -
      defaultMovieLayout->residentBytes + 1U;
  auto sharedBudgetMovie = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = movieModel,
       .configuration = configuration,
       .device = sharedBudgetMovieDevice,
       .sessionDecodedBytes = sharedInitialBytes});
  expect(!sharedBudgetMovie.catalog && sharedBudgetMovieDevice->loads == 1 &&
             sharedBudgetMovieDevice->expensiveAllocations == 0 &&
             sharedBudgetMovieDevice->live.empty(),
         "ordinary skin decoded bytes reduce the movie load allowance before "
         "any codec allocation");

  skin::ValidatedBeatorajaSkinModel twoMovieModel = movieModel;
  twoMovieModel.model.resources.emplace_back(skin::SkinMovieResource{
      .id = 23, .virtualPath = "resources/second.webm", .authoredOrdinal = 3});
  twoMovieModel.model.objects.push_back(
      {.id = 23,
       .authoredName = "movie-three",
       .payload = skin::SkinImageObject{.orderedStates = {{.resource = 23}}},
       .critical = true});
  auto aggregateBudgetMovieDevice = std::make_shared<FakeMovieDevice>();
  const std::size_t oneMovieShortInitialBytes =
      skin::SkinResourcePolicy::maximumSessionDecodedBytes -
      defaultMovieLayout->residentBytes * 2U + 1U;
  auto aggregateBudgetMovies = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = twoMovieModel,
       .configuration = configuration,
       .device = aggregateBudgetMovieDevice,
       .sessionDecodedBytes = oneMovieShortInitialBytes});
  expect(!aggregateBudgetMovies.catalog &&
             aggregateBudgetMovieDevice->loads == 2 &&
             aggregateBudgetMovieDevice->expensiveAllocations == 1 &&
             aggregateBudgetMovieDevice->destroys == 1 &&
             aggregateBudgetMovieDevice->live.empty(),
         "each unique live movie consumes the shared decoded budget and a "
         "later rejection rolls back the earlier player");

  auto failingMovieDevice = std::make_shared<FakeMovieDevice>();
  failingMovieDevice->failAt = 2;
  auto failedMovies = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = twoMovieModel,
       .configuration = configuration,
       .device = failingMovieDevice});
  expect(!failedMovies.catalog && !failedMovies.cancelled &&
             failingMovieDevice->loads == 2 &&
             failingMovieDevice->destroys == 1 &&
             failingMovieDevice->live.empty() &&
             std::ranges::all_of(failingMovieDevice->loadedPaths,
                                 [](const auto &path) {
                                   return !std::filesystem::exists(path);
                                 }),
         "a failed movie creation rolls back every prior player and materialized source exactly once");

  std::stop_source movieStop;
  auto cancelledMovieDevice = std::make_shared<FakeMovieDevice>();
  cancelledMovieDevice->stopAfterLoad = &movieStop;
  auto cancelledMovies = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leasedFs.fileSystem,
       .model = movieModel,
       .configuration = configuration,
       .device = cancelledMovieDevice,
       .stop = movieStop.get_token()});
  expect(!cancelledMovies.catalog && cancelledMovies.cancelled &&
             cancelledMovieDevice->loads == 1 &&
             cancelledMovieDevice->destroys == 1 &&
             cancelledMovieDevice->live.empty() &&
             !fs::exists(cancelledMovieDevice->loadedPaths.front()),
         "cancellation after movie load prevents publication and tears down the player and file");
  skin::ValidatedBeatorajaSkinModel emptyModel;
  std::stop_source preStoppedCaller;
  preStoppedCaller.request_stop();
  const auto preStoppedValidation = service.validateResources(
      {.revision = lease->readView(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = emptyModel,
       .configuration = configuration,
       .stop = preStoppedCaller.get_token()});
  expect(preStoppedValidation.cancelled && !preStoppedValidation.valid,
         "a caller stop observed only at the final validation boundary prevents publication");
  const auto preStoppedPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = emptyModel,
       .configuration = configuration,
       .stop = preStoppedCaller.get_token()});
  expect(preStoppedPlan.cancelled && !preStoppedPlan.plan,
         "a caller stop observed only at the final plan boundary prevents publication");
  {
  const auto configuredImageModel = [](std::string explicitPath) {
    skin::ValidatedBeatorajaSkinModel configured;
    configured.model.resources.emplace_back(skin::SkinImageResource{
        .id=1, .authoredName="configured", .virtualPath="resources/image-*"});
    configured.model.resources.emplace_back(skin::SkinImageResource{
        .id=2, .authoredName="explicit", .virtualPath=std::move(explicitPath)});
    configured.model.objects.push_back(
        {.id=1, .authoredName="configured-image",
         .payload=skin::SkinImageObject{.orderedStates={{.resource=1, .frames={{.x=0,.y=0,.w=-1,.h=-1}}}}},
         .critical=true});
    configured.model.objects.push_back(
        {.id=2, .authoredName="explicit-image",
         .payload=skin::SkinImageObject{.orderedStates={{.resource=2, .frames={{.x=0,.y=0,.w=-1,.h=-1}}}}},
         .critical=true});
    return configured;
  };
  skin::BeatorajaSkinConfiguration defaultFileConfiguration;
  defaultFileConfiguration.orderedFiles.push_back(
      {.name="Image", .pattern="resources/image-*",
       .selectedValue="default.ppm"});
  const auto defaultConfiguredPlan = service.decodeAndPlan(
      {.revision=lease->clone(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem,
       .model=configuredImageModel("resources/image-default.ppm"),
       .configuration=defaultFileConfiguration});
  expect(defaultConfiguredPlan.plan &&
             defaultConfiguredPlan.plan->images.size() == 1 &&
             defaultConfiguredPlan.plan->images.front().pixels.width == 1 &&
             defaultConfiguredPlan.plan->images.front().aliases ==
                 std::vector<skin::SkinResourceId>{2},
         "configured default image substitution deduplicates against an explicit resolved path");
  writePpm(source / "entry/resources/image-default.ppm", 3);
  const auto changedLiveResourcePlan = service.decodeAndPlan(
      {.revision=lease->clone(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem,
       .model=configuredImageModel("resources/image-default.ppm"),
       .configuration=defaultFileConfiguration});
  expect(changedLiveResourcePlan.plan &&
             changedLiveResourcePlan.plan->images.size() == 1 &&
             changedLiveResourcePlan.plan->images.front().pixels.width == 3,
         "a Files-visible skin resource edit bypasses stale decoded pixels");
  skin::BeatorajaSkinConfiguration selectedFileConfiguration;
  selectedFileConfiguration.orderedFiles.push_back(
      {.name="Image", .pattern="resources/image-*",
       .selectedValue="selected.ppm"});
  const auto selectedConfiguredPlan = service.decodeAndPlan(
      {.revision=lease->clone(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem,
       .model=configuredImageModel("resources/image-selected.ppm"),
       .configuration=selectedFileConfiguration});
  expect(selectedConfiguredPlan.plan &&
             selectedConfiguredPlan.plan->images.size() == 1 &&
             selectedConfiguredPlan.plan->images.front().pixels.width == 2 &&
             selectedConfiguredPlan.plan->images.front().aliases ==
                 std::vector<skin::SkinResourceId>{2},
         "configured selected image substitution changes the physical cache key without remapping an explicit path");
  skin::ValidatedBeatorajaSkinModel configuredFontModel;
  configuredFontModel.model.resources.emplace_back(skin::SkinFontResource{
      .id=1, .authoredName="configured-font",
      .virtualPath="resources/font-primary-*", .type=0,
      .fallbacks={{.virtualPath="", .type=0},
                  {.virtualPath="resources/font-fallback-*", .type=0},
                  {.virtualPath="", .type=0},
                  {.virtualPath="resources/font-secondary-*", .type=0},
                  {.virtualPath="", .type=0}}});
  configuredFontModel.model.objects.push_back(
      {.id=1, .authoredName="configured-font-text",
       .payload=skin::SkinTextObject{.font=1, .literal="A", .pointSize=16},
       .critical=true});
  skin::BeatorajaSkinConfiguration configuredFonts;
  configuredFonts.orderedFiles = {
      {.name="Primary", .pattern="resources/font-primary-*",
       .selectedValue="selected.ttf"},
      {.name="Fallback", .pattern="resources/font-fallback-*",
       .selectedValue="selected.ttf"},
      {.name="Secondary", .pattern="resources/font-secondary-*",
       .selectedValue="selected.otf"}};
  const auto configuredFontPlan = service.decodeAndPlan(
      {.revision=lease->clone(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem, .model=configuredFontModel,
       .configuration=configuredFonts});
  const auto configuredDigest = configuredFontPlan.plan &&
          configuredFontPlan.plan->atlases.size() == 1
      ? configuredFontPlan.plan->atlases.front().key.fallbackChainDigest
      : std::string{};
  const auto primaryPosition = configuredDigest.find("font-primary-selected.ttf");
  const auto fallbackPosition = configuredDigest.find("font-fallback-selected.ttf");
  const auto secondaryPosition = configuredDigest.find("font-secondary-selected.otf");
  expect(configuredFontPlan.plan && configuredFontPlan.plan->atlases.size() == 1 &&
             primaryPosition != std::string::npos &&
             fallbackPosition > primaryPosition &&
             secondaryPosition > fallbackPosition,
         "configured primary and nonempty fallback font slots resolve once in authored order while empty placeholders are skipped");
  skin::BeatorajaSkinConfiguration ambiguousFiles = defaultFileConfiguration;
  ambiguousFiles.orderedFiles.push_back(
      {.name="Image duplicate", .pattern="resources/image-*",
       .selectedValue="selected.ppm"});
  const auto ambiguousConfiguration = service.validateResources(
      {.revision=lease->readView(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem,
       .model=configuredImageModel("resources/image-default.ppm"),
       .configuration=ambiguousFiles});
  expect(ambiguousConfiguration.valid &&
             !hasDiagnostic(ambiguousConfiguration.diagnostics,
                            "skin.resource.configuration_ambiguous"),
         "overlapping configured file matches retain SkinLoader's first "
         "filemap selection");
  skin::BeatorajaSkinConfiguration oversizedSelection =
      defaultFileConfiguration;
  oversizedSelection.orderedFiles.front().selectedValue.assign(1025, 'x');
  const auto oversizedConfiguredPath = service.validateResources(
      {.revision=lease->readView(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem,
       .model=configuredImageModel("resources/image-default.ppm"),
       .configuration=oversizedSelection});
  expect(!oversizedConfiguredPath.valid &&
             hasDiagnostic(oversizedConfiguredPath.diagnostics,
                           "skin.resource.configuration_ambiguous"),
         "configured file substitution enforces the host package-path bound before allocation");
  }
  skin::ValidatedBeatorajaSkinModel distributedLogicalResources;
  for (skin::SkinResourceId id = 1;
       id <= skin::SkinResourcePolicy::maximumResources + 1;
       ++id) {
    distributedLogicalResources.model.resources.emplace_back(
        skin::SkinImageResource{.id=id, .authoredName="distributed",
                                .virtualPath="resources/fixture.png"});
    distributedLogicalResources.model.objects.push_back(
        {.id=id, .authoredName="distributed-image",
         .payload=skin::SkinImageObject{.orderedStates={{
             .resource=id, .frames={{.x=0,.y=0,.w=40,.h=20}}}}},
         .critical=true});
  }
  const auto distributedValidation = service.validateResources(
      {.revision=lease->readView(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem, .model=distributedLogicalResources,
       .configuration=configuration});
  expect(!distributedValidation.valid &&
             hasDiagnostic(distributedValidation.diagnostics,
                           "skin.resource.session_limit"),
         "validation rejects a distributed 513th logical resource before upload");
  const auto distributedPlan = service.decodeAndPlan(
      {.revision=lease->clone(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem, .model=distributedLogicalResources,
       .configuration=configuration});
  expect(!distributedPlan.plan &&
             hasDiagnostic(distributedPlan.diagnostics,
                           "skin.resource.session_limit"),
         "planning rejects the same distributed logical-resource overage before upload");
  auto unrestrictedDistributedPlan = service.decodeAndPlan(
      {.revision=lease->clone(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem, .model=distributedLogicalResources,
       .configuration=configuration,
       .safetyPolicy=skin::SkinSafetyPolicy(
           skin::SkinSafetyLevel::Unrestricted)});
  expect(unrestrictedDistributedPlan.plan &&
             unrestrictedDistributedPlan.plan->images.size() == 1 &&
             unrestrictedDistributedPlan.plan->images.front().aliases.size() ==
                 skin::SkinResourcePolicy::maximumResources,
         "Unrestricted retains otherwise-valid resources beyond the Standard session limit");
  auto unrestrictedDevice = std::make_shared<FakeTextureDevice>();
  auto unrestrictedUpload = skin::SkinResourceCatalog::upload(
      std::move(*unrestrictedDistributedPlan.plan), unrestrictedDevice);
  unrestrictedDistributedPlan.plan.reset();
  expect(unrestrictedUpload.catalog && unrestrictedDevice->creates == 1 &&
             unrestrictedUpload.catalog->find(
                 skin::SkinResourcePolicy::maximumResources + 1),
         "the prepared Unrestricted policy also bypasses upload-time session limits");
  unrestrictedUpload.catalog.reset();
  skin::ValidatedBeatorajaSkinModel physicalResourceCapacity;
  for (skin::SkinResourceId id = 1;
       id <= skin::SkinResourcePolicy::maximumResources + 1;
       ++id) {
    physicalResourceCapacity.model.resources.emplace_back(
        skin::SkinImageResource{.id=id, .authoredName="physical-capacity",
                                .virtualPath="resources/cap-" +
                                             std::to_string(id - 1) + ".ppm"});
    physicalResourceCapacity.model.objects.push_back(
        {.id=id, .authoredName="physical-capacity-image",
         .payload=skin::SkinImageObject{.orderedStates={{
             .resource=id, .frames={{.x=0,.y=0,.w=1,.h=1}}}}},
         .critical=true});
  }
  std::atomic_int capacityDecoderCalls = 0;
  auto capacityPixels = std::make_shared<std::vector<unsigned char>>(4, 255);
  skin::SkinResourcePreparationService capacityService(
      [&](std::span<const std::byte>, std::stop_token stop)
          -> std::optional<image_decode::DecodedImageData> {
        if (stop.stop_requested()) return std::nullopt;
        ++capacityDecoderCalls;
        return image_decode::DecodedImageData{
            .width=1, .height=1, .rgba=capacityPixels};
      });
  const auto physicalCapacityPlan = capacityService.decodeAndPlan(
      {.revision=lease->clone(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem, .model=physicalResourceCapacity,
       .configuration=configuration});
  expect(!physicalCapacityPlan.plan &&
             hasDiagnostic(physicalCapacityPlan.diagnostics,
                           "skin.resource.session_limit") &&
             capacityDecoderCalls ==
                 static_cast<int>(skin::SkinResourcePolicy::maximumResources),
         "the physical 513th resource is rejected after its bounded read but before decode work is queued");
  skin::ValidatedBeatorajaSkinModel atlasRequestCapacity;
  atlasRequestCapacity.model.resources.emplace_back(
      skin::SkinFontResource{.id=1, .authoredName="capacity-font",
                             .virtualPath="resources/fixture.ttf", .type=0});
  for (int index = 0;
       index <= static_cast<int>(skin::SkinResourcePolicy::maximumAtlases);
       ++index) {
    atlasRequestCapacity.model.objects.push_back(
        {.id=static_cast<skin::SkinObjectId>(index + 1),
         .authoredName="capacity-text",
         .payload=skin::SkinTextObject{.font=1, .literal="A",
                                        .pointSize=16 + index},
         .critical=index < static_cast<int>(skin::SkinResourcePolicy::maximumAtlases)});
  }
  skin::resetSkinResourceFontAtlasRequestHighWaterForTesting();
  auto compactAtlasPlan = service.decodeAndPlan(
      {.revision=lease->clone(), .entry=entry,
       .fileSystem=*leasedFs.fileSystem, .model=atlasRequestCapacity,
       .configuration=configuration});
  expect(compactAtlasPlan.plan &&
             compactAtlasPlan.plan->atlases.size() ==
                 skin::SkinResourcePolicy::maximumAtlases &&
             compactAtlasPlan.plan->atlases.front().id == 1 &&
             compactAtlasPlan.plan->atlases.back().id ==
                 skin::SkinResourcePolicy::maximumAtlases &&
             skin::skinResourceFontAtlasRequestHighWaterForTesting() <=
                 skin::SkinResourcePolicy::maximumAtlases &&
             hasDiagnostic(compactAtlasPlan.diagnostics,
                           "skin.resource.atlas_limit"),
         "the optional rejected atlas request is not retained and performs no ID allocation, preserving compact key-order atlas IDs");
  compactAtlasPlan.plan.reset();
  auto planned = service.decodeAndPlan({.revision=std::move(*lease), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(planned.plan && planned.plan->images.size() == 2 && planned.plan->atlases.size() == 8 &&
             planned.plan->textAtlasesByObject.size() == 9 &&
             planned.plan->textAtlasesByObject.contains(6) &&
             planned.plan->textAtlasesByObject.contains(7) &&
             planned.plan->textAtlasesByObject.contains(8) &&
             planned.plan->textAtlasesByObject.contains(9) &&
             planned.plan->textAtlasesByObject.contains(10) &&
             planned.plan->textAtlasesByObject.contains(11) &&
             planned.plan->textAtlasesByObject.contains(12) &&
             planned.plan->textAtlasesByObject.contains(13) &&
             planned.plan->textAtlasesByObject.contains(14) &&
             planned.plan->images.front().aliases == std::vector<skin::SkinResourceId>{2},
         "duplicate image locators reuse one texture while every live text object publishes its prepared atlas identity");
  std::optional<skin::SkinTextAtlasKey> firstAtlasKey;
  if (planned.plan && planned.plan->atlases.size() == 8) {
    const auto &firstAtlas = planned.plan->atlases[0];
    const auto &secondAtlas = planned.plan->atlases[1];
    firstAtlasKey = firstAtlas.key;
    bool styledColor = false;
    for (const unsigned char component : *secondAtlas.pixels.rgba)
      if (component != 0 && component != 255) { styledColor = true; break; }
    const auto signikaAtlas = std::find_if(planned.plan->atlases.begin(), planned.plan->atlases.end(),
        [](const skin::SkinPreparedGlyphAtlas &atlas) { return atlas.key.font == 9; });
    const auto bitmapAtlas = std::find_if(planned.plan->atlases.begin(), planned.plan->atlases.end(),
        [](const skin::SkinPreparedGlyphAtlas &atlas) { return atlas.key.font == 10; });
    const auto distanceAtlas = std::find_if(planned.plan->atlases.begin(), planned.plan->atlases.end(),
        [](const skin::SkinPreparedGlyphAtlas &atlas) { return atlas.key.font == 11; });
    const auto coloredDistanceAtlas = std::find_if(planned.plan->atlases.begin(), planned.plan->atlases.end(),
        [](const skin::SkinPreparedGlyphAtlas &atlas) { return atlas.key.font == 12; });
    const auto bitmapFallbackAtlas = std::find_if(planned.plan->atlases.begin(), planned.plan->atlases.end(),
        [](const skin::SkinPreparedGlyphAtlas &atlas) { return atlas.key.font == 13; });
    const auto &firstAGlyph = firstAtlas.glyphs.at(U'A');
    expect(firstAtlas.id == 1 && secondAtlas.id == 2 && firstAtlas.key.pointSize == 16 &&
               secondAtlas.key.pointSize == 24 && firstAtlas.glyphs.contains(U'日') &&
               firstAtlas.glyphs.contains(U'4') && firstAtlas.glyphs.at(U'A').region.x > 0 &&
               firstAtlas.capHeight > 0 &&
                firstAGlyph.layoutOffsetY == firstAGlyph.bearingY -
                                                   firstAGlyph.region.h -
                                                   firstAtlas.capHeight &&
               static_cast<double>(firstAtlas.glyphs.at(U'A').region.x) / firstAtlas.pixels.width > 0.0 &&
               static_cast<double>(firstAtlas.glyphs.at(U'A').region.x) / firstAtlas.pixels.width < 1.0 && styledColor &&
               signikaAtlas != planned.plan->atlases.end() && signikaAtlas->kerning.contains({U'A', U'V'}) &&
               signikaAtlas->kerning.at({U'A', U'V'}) != 0 &&
               bitmapAtlas != planned.plan->atlases.end() && bitmapAtlas->bitmapFont &&
               bitmapAtlas->bitmapFontType == 0 && bitmapAtlas->originalSize == 10 &&
               bitmapAtlas->glyphs.contains(U'\U0001f642') &&
               bitmapAtlas->pages.size() == 2 &&
               bitmapAtlas->glyphs.at(U'V').page == 1 &&
               bitmapAtlas->glyphs.at(U'V').region.x == 15 &&
               bitmapAtlas->kerning.at({U'A', U'V'}) == -2 &&
               distanceAtlas != planned.plan->atlases.end() &&
               distanceAtlas->bitmapFontType == 1 &&
               coloredDistanceAtlas != planned.plan->atlases.end() &&
               coloredDistanceAtlas->bitmapFontType == 2 &&
               bitmapFallbackAtlas != planned.plan->atlases.end() &&
               bitmapFallbackAtlas->glyphs.contains(U'\u301c'),
           "prepared font atlases have stable keys, normalized UV regions, styled pixels, Japanese/runtime glyphs, fallback selection, and real AV kerning");
  }
  if (!planned.plan) return;
  const auto copyUploadPlan = [&] {
    return skin::SkinResourceUploadPlan{
        .revision = planned.plan->revision.clone(),
        .images = planned.plan->images,
        .atlases = planned.plan->atlases,
        .textAtlasesByObject = planned.plan->textAtlasesByObject,
        .decodedBytes = planned.plan->decodedBytes};
  };
  const auto rejectBeforeUpload = [&](std::string_view message,
                                      const auto &mutate) {
    auto rejectedPlan = copyUploadPlan();
    auto preflightDevice = std::make_shared<FakeTextureDevice>();
    mutate(rejectedPlan, *preflightDevice);
    const auto rejected = skin::SkinResourceCatalog::upload(
        std::move(rejectedPlan), preflightDevice);
    expect(!rejected.catalog && preflightDevice->creates == 0 &&
               preflightDevice->live == 0,
           message);
  };
  const auto sharedSixteenMiBPixels = [] {
    auto rgba = std::make_shared<std::vector<unsigned char>>(16U * 1024U *
                                                              1024U);
    return image_decode::DecodedImageData{.width = 2048,
                                          .height = 2048,
                                          .rgba = std::move(rgba)};
  };
  rejectBeforeUpload("zero device texture limit fails before upload or lease move",
                     [](auto &, auto &device) { device.maximumDimension = 0; });
  rejectBeforeUpload("effective device texture limit bounds every decoded image before upload",
                     [](auto &, auto &device) { device.maximumDimension = 1; });
  rejectBeforeUpload("resource-count cap rejects shared-pixel synthetic plans before upload",
                     [](auto &plan, auto &) {
                       const auto image = plan.images.front();
                       plan.images.clear();
                       for (skin::SkinResourceId id = 1;
                            id <= skin::SkinResourcePolicy::maximumResources;
                            ++id) {
                         auto copy = image;
                         copy.id = id;
                         copy.aliases.clear();
                         copy.aliasRegions.clear();
                         copy.aliasRegionMappings.clear();
                         plan.images.push_back(std::move(copy));
                       }
                       auto overflow = image;
                       overflow.id = skin::SkinResourcePolicy::maximumResources + 1;
                       overflow.aliases.clear();
                       overflow.aliasRegions.clear();
                       overflow.aliasRegionMappings.clear();
                       plan.images.push_back(std::move(overflow));
                     });
  rejectBeforeUpload("alias-count cap rejects before upload",
                     [](auto &plan, auto &) {
                       auto &image = plan.images.front();
                       image.aliases.clear();
                       image.aliasRegions.clear();
                       image.aliasRegionMappings.clear();
                       for (skin::SkinResourceId id = 4;
                            image.aliases.size() < skin::SkinResourcePolicy::maximumResources;
                            ++id) {
                         image.aliases.push_back(id);
                       }
                     });
  rejectBeforeUpload("logical resource cap cannot underflow after aliases fill the ID set",
                     [](auto &plan, auto &) {
                       auto &first = plan.images.front();
                       first.aliases.clear();
                       first.aliasRegions.clear();
                       first.aliasRegionMappings.clear();
                       for (skin::SkinResourceId id = 2;
                            id <= skin::SkinResourcePolicy::maximumResources;
                            ++id) {
                         first.aliases.push_back(id);
                         first.aliasRegionMappings.emplace(id,
                                                           first.regionMappings);
                       }
                       auto &next = plan.images.back();
                       next.id = skin::SkinResourcePolicy::maximumResources + 1;
                       next.aliases.clear();
                       next.aliasRegions.clear();
                       next.aliasRegionMappings.clear();
                     });
  rejectBeforeUpload("duplicate image aliases fail complete preflight before upload",
                     [](auto &plan, auto &) {
                       plan.images.front().aliases.push_back(plan.images.back().id);
                     });
  rejectBeforeUpload("decoded image dimensions and shared RGBA byte count must agree",
                     [](auto &plan, auto &) { ++plan.images.front().pixels.width; });
  rejectBeforeUpload("canonical primary regions are verified before upload",
                     [](auto &plan, auto &) { plan.images.front().regions.front().x = -1; });
  rejectBeforeUpload("primary authored-to-resolved mappings cannot be substituted",
                     [](auto &plan, auto &) { ++plan.images.front().regionMappings.front().resolved.x; });
  rejectBeforeUpload("canonical alias regions are verified before upload",
                     [](auto &plan, auto &) { plan.images.front().aliasRegions.begin()->second.front().x = -1; });
  rejectBeforeUpload("alias authored-to-resolved mappings cannot be substituted",
                     [](auto &plan, auto &) { ++plan.images.front().aliasRegionMappings.begin()->second.front().resolved.x; });
  rejectBeforeUpload("aggregate image decoded-byte accounting rejects shared pixels before upload",
                     [&](auto &plan, auto &) {
                       const auto templateImage = plan.images.front();
                       const auto pixels = sharedSixteenMiBPixels();
                       plan.images.clear();
                       plan.atlases.clear();
                       for (skin::SkinResourceId id = 1; id <= 17; ++id) {
                         auto image = templateImage;
                         image.id = id;
                         image.aliases.clear();
                         image.aliasRegions.clear();
                         image.aliasRegionMappings.clear();
                         image.pixels = pixels;
                         plan.images.push_back(std::move(image));
                       }
                     });
  rejectBeforeUpload("upload plan decoded-byte totals must exactly match shared pixels",
                     [](auto &plan, auto &) { ++plan.decodedBytes; });
  rejectBeforeUpload("atlas-count cap rejects shared-pixel synthetic plans before upload",
                     [](auto &plan, auto &) {
                       const auto atlas = plan.atlases.front();
                       plan.atlases.clear();
                       for (skin::SkinTextAtlasId id = 1;
                            id <= skin::SkinResourcePolicy::maximumAtlases;
                            ++id) {
                         auto copy = atlas;
                         copy.id = id;
                         copy.key.pointSize += static_cast<int>(id);
                         plan.atlases.push_back(std::move(copy));
                       }
                       auto overflow = atlas;
                       overflow.id = skin::SkinResourcePolicy::maximumAtlases + 1;
                       overflow.key.pointSize +=
                           static_cast<int>(skin::SkinResourcePolicy::maximumAtlases + 1);
                       plan.atlases.push_back(std::move(overflow));
                     });
  rejectBeforeUpload("duplicate atlas IDs fail before upload",
                     [](auto &plan, auto &) { plan.atlases.back().id = plan.atlases.front().id; });
  rejectBeforeUpload("aggregate scalable-font paint work fails before upload",
                     [](auto &plan, auto &) {
                       for (auto &atlas : plan.atlases) {
                         atlas.paintBlendOperations = 0;
                       }
                       plan.atlases[0].paintBlendOperations =
                           40U * 1024U * 1024U;
                       plan.atlases[1].paintBlendOperations =
                           24U * 1024U * 1024U + 1U;
                     });
  rejectBeforeUpload("bitmap atlases cannot claim scalable paint work",
                     [](auto &plan, auto &) {
                       const auto bitmap = std::ranges::find_if(
                           plan.atlases,
                           [](const auto &atlas) { return atlas.bitmapFont; });
                       bitmap->paintBlendOperations = 1;
                     });
  rejectBeforeUpload("text-atlas mappings must reference an uploaded atlas",
                     [](auto &plan, auto &) {
                       plan.textAtlasesByObject.begin()->second =
                           skin::SkinResourcePolicy::maximumAtlases + 1;
                     });
  rejectBeforeUpload("text-atlas mappings require nonzero object IDs",
                     [](auto &plan, auto &) {
                       const auto atlas = plan.textAtlasesByObject.begin()->second;
                       plan.textAtlasesByObject.emplace(0, atlas);
                     });
  rejectBeforeUpload("atlas styles must retain canonical finite keys",
                     [](auto &plan, auto &) { plan.atlases.front().key.outlineWidth = -1.0; });
  rejectBeforeUpload("oversized atlas fallback-chain keys fail before upload",
                     [](auto &plan, auto &) {
                       plan.atlases.front().key.fallbackChainDigest.assign(
                           65537, 'x');
                     });
  rejectBeforeUpload("atlas dimensions and shared RGBA byte counts must agree",
                     [](auto &plan, auto &) { ++plan.atlases.front().pixels.width; });
  rejectBeforeUpload("aggregate atlas decoded-byte accounting rejects shared pixels before upload",
                     [&](auto &plan, auto &) {
                       const auto templateAtlas = plan.atlases.front();
                       const auto pixels = sharedSixteenMiBPixels();
                       plan.atlases.clear();
                       for (skin::SkinTextAtlasId id = 1; id <= 9; ++id) {
                         auto atlas = templateAtlas;
                         atlas.id = id;
                         atlas.key.pointSize += static_cast<int>(id);
                         atlas.pixels = pixels;
                         plan.atlases.push_back(std::move(atlas));
                       }
                     });
  rejectBeforeUpload("atlas kerning pairs must reference prepared glyphs",
                     [](auto &plan, auto &) { plan.atlases.front().kerning[{U'\U0010ffff', U'\U0010ffff'}] = 1; });
  rejectBeforeUpload("INT_MIN kerning is rejected before upload",
                     [](auto &plan, auto &) {
                       const auto atlas = std::find_if(
                           plan.atlases.begin(), plan.atlases.end(),
                           [](const auto &item) { return !item.kerning.empty(); });
                       atlas->kerning.begin()->second =
                           std::numeric_limits<int>::min();
                     });
  rejectBeforeUpload("INT_MAX kerning is rejected before upload",
                     [](auto &plan, auto &) {
                       const auto atlas = std::find_if(
                           plan.atlases.begin(), plan.atlases.end(),
                           [](const auto &item) { return !item.kerning.empty(); });
                       atlas->kerning.begin()->second =
                           std::numeric_limits<int>::max();
                     });
  {
    auto signedKerningPlan = copyUploadPlan();
    const auto preparedAtlas = std::find_if(
        signedKerningPlan.atlases.begin(), signedKerningPlan.atlases.end(),
        [](const auto &item) { return !item.kerning.empty(); });
    const auto atlasId = preparedAtlas->id;
    const auto pair = preparedAtlas->kerning.begin()->first;
    const int preparedCapHeight = preparedAtlas->capHeight;
    const int preparedLayoutOffset =
        preparedAtlas->glyphs.begin()->second.layoutOffsetY;
    const int signedAmount = -skin::SkinResourcePolicy::maximumDimension;
    preparedAtlas->kerning.begin()->second = signedAmount;
    auto signedDevice = std::make_shared<FakeTextureDevice>();
    const auto signedUpload = skin::SkinResourceCatalog::upload(
        std::move(signedKerningPlan), signedDevice);
    const auto *uploadedAtlas = signedUpload.catalog
        ? signedUpload.catalog->findTextAtlas(atlasId)
        : nullptr;
    expect(uploadedAtlas && uploadedAtlas->kerning.at(pair) == signedAmount &&
               uploadedAtlas->capHeight == preparedCapHeight &&
               uploadedAtlas->glyphs.begin()->second.layoutOffsetY ==
                   preparedLayoutOffset,
           "valid signed kerning and vertical layout metrics are preserved through catalog upload");
  }
  rejectBeforeUpload("atlas metrics stay within fixed policy bounds",
                     [](auto &plan, auto &) { plan.atlases.front().lineHeight = skin::SkinResourcePolicy::maximumDimension + 1; });
  rejectBeforeUpload("atlas cap height must remain positive",
                     [](auto &plan, auto &) {
                       plan.atlases.front().capHeight = 0;
                     });
  rejectBeforeUpload("atlas glyph layout offsets stay within fixed bounds",
                     [](auto &plan, auto &) {
                       plan.atlases.front().glyphs.begin()->second.layoutOffsetY =
                           skin::SkinResourcePolicy::maximumDimension + 1;
                     });
  rejectBeforeUpload("atlas glyph-count cap rejects before upload",
                     [](auto &plan, auto &) {
                       auto &glyphs = plan.atlases.front().glyphs;
                       const auto metric = glyphs.begin()->second;
                       for (char32_t codepoint = U'\U0010fffe';
                            glyphs.size() <= skin::SkinResourcePolicy::maximumGlyphs;
                            --codepoint) {
                         glyphs.emplace(codepoint, metric);
                       }
                     });
  rejectBeforeUpload("atlas kerning-count cap rejects before upload",
                     [](auto &plan, auto &) {
                       auto &atlas = plan.atlases.front();
                       const auto metric = atlas.glyphs.begin()->second;
                       std::vector<char32_t> codepoints;
                       for (char32_t codepoint = U'\u1000'; codepoint < U'\u1082'; ++codepoint) {
                         atlas.glyphs.emplace(codepoint, metric);
                         codepoints.push_back(codepoint);
                       }
                       for (const char32_t left : codepoints) {
                         for (const char32_t right : codepoints) {
                           atlas.kerning.emplace(std::pair{left, right}, 0);
                           if (atlas.kerning.size() > skin::SkinResourcePolicy::maximumKerningPairs) return;
                         }
                       }
                     });
  rejectBeforeUpload("aggregate glyph-count cap rejects otherwise valid atlases before upload",
                     [](auto &plan, auto &) {
                       auto first = plan.atlases.front();
                       auto second = first;
                       second.id = first.id + 1;
                       ++second.key.pointSize;
                       for (auto *atlas : {&first, &second}) {
                         const auto metric = atlas->glyphs.begin()->second;
                         for (char32_t codepoint = U'\u3000';
                              atlas->glyphs.size() < 5000;
                              ++codepoint) {
                           atlas->glyphs.emplace(codepoint, metric);
                         }
                       }
                       plan.atlases = {std::move(first), std::move(second)};
                     });
  rejectBeforeUpload("aggregate kerning-count cap rejects otherwise valid atlases before upload",
                     [](auto &plan, auto &) {
                       auto first = plan.atlases.front();
                       auto second = first;
                       second.id = first.id + 1;
                       ++second.key.pointSize;
                       for (auto *atlas : {&first, &second}) {
                         const auto metric = atlas->glyphs.begin()->second;
                         std::vector<char32_t> codepoints;
                         for (char32_t codepoint = U'\u4000'; codepoint < U'\u4064'; ++codepoint) {
                           atlas->glyphs.emplace(codepoint, metric);
                           codepoints.push_back(codepoint);
                         }
                         for (const char32_t left : codepoints) {
                           for (const char32_t right : codepoints) {
                             atlas->kerning.emplace(std::pair{left, right}, 0);
                           }
                         }
                       }
                       plan.atlases = {std::move(first), std::move(second)};
                     });
  {
    skin::SkinResourceUploadPlan duplicatePlan{
        .revision = planned.plan->revision.clone(), .images = planned.plan->images,
        .atlases = planned.plan->atlases,
        .decodedBytes = planned.plan->decodedBytes};
    duplicatePlan.images.front().aliases.push_back(duplicatePlan.images.front().id);
    auto preflightDevice = std::make_shared<FakeTextureDevice>();
    const auto duplicateUpload = skin::SkinResourceCatalog::upload(std::move(duplicatePlan), preflightDevice);
    expect(!duplicateUpload.catalog && preflightDevice->creates == 0,
           "duplicate upload IDs fail complete preflight before any texture creation");
  }
  {
    skin::SkinResourceUploadPlan malformedMappingPlan{
        .revision = planned.plan->revision.clone(), .images = planned.plan->images,
        .atlases = planned.plan->atlases,
        .decodedBytes = planned.plan->decodedBytes};
    malformedMappingPlan.images.front().regionMappings.front().authored.x = -1;
    auto preflightDevice = std::make_shared<FakeTextureDevice>();
    const auto malformedMappingUpload =
        skin::SkinResourceCatalog::upload(std::move(malformedMappingPlan),
                                          preflightDevice);
    expect(!malformedMappingUpload.catalog && preflightDevice->creates == 0,
           "noncanonical authored mapping fails before upload or lease move");
  }
  {
    skin::ValidatedBeatorajaSkinModel uniquePlanningModel;
    uniquePlanningModel.model.resources.emplace_back(skin::SkinImageResource{
        .id=1, .authoredName="unique", .virtualPath="resources/fixture.png"});
    skin::SkinSpriteFrames uniqueFrames{.resource=1};
    constexpr std::size_t uniqueCount = 100000;
    uniqueFrames.frames.reserve(uniqueCount);
    for (int columns = 1;
         columns <= 40 && uniqueFrames.frames.size() < uniqueCount;
         ++columns) {
      for (int rows = 1;
           rows <= 20 && uniqueFrames.frames.size() < uniqueCount; ++rows) {
        for (int column = 0;
             column < columns && uniqueFrames.frames.size() < uniqueCount;
             ++column) {
          for (int row = 0;
               row < rows && uniqueFrames.frames.size() < uniqueCount; ++row) {
            uniqueFrames.frames.push_back(
                {.x=0, .y=0, .w=40, .h=20, .gridColumn=column,
                 .gridRow=row, .gridColumns=columns, .gridRows=rows});
          }
        }
      }
    }
    expect(uniqueFrames.frames.size() == uniqueCount,
           "the deterministic grid fixture contains 100000 unique authored rectangles");
    uniquePlanningModel.model.objects.push_back(
        {.id=1, .authoredName="unique-image",
         .payload=skin::SkinImageObject{.orderedStates={uniqueFrames}},
         .critical=true});
    skin::resetSkinResourceRegionIdentityChecksForTesting();
    auto uniquePlan = service.decodeAndPlan(
        {.revision=planned.plan->revision.clone(), .entry=entry,
         .fileSystem=*leasedFs.fileSystem, .model=uniquePlanningModel,
         .configuration=configuration});
    const std::size_t planningComparisons =
        skin::skinResourceRegionIdentityChecksForTesting();
    expect(uniquePlan.plan && uniquePlan.plan->images.size() == 1 &&
               uniquePlan.plan->images.front().regions.size() == uniqueCount &&
               uniquePlan.plan->images.front().regions[0].w == 40 &&
               uniquePlan.plan->images.front().regions[1].h == 10 &&
               planningComparisons > uniqueCount &&
               planningComparisons <= uniqueCount * 40,
           "planning preserves unique authored mapping order with measured logarithmic map comparisons");
    if (uniquePlan.plan) {
      auto &image = uniquePlan.plan->images.front();
      image.aliases = {2};
      image.aliasRegions.emplace(2, image.regions);
      image.aliasRegionMappings.emplace(2, image.regionMappings);
      skin::resetSkinResourceRegionIdentityChecksForTesting();
      auto uniqueDevice = std::make_shared<FakeTextureDevice>();
      const auto uniqueUpload = skin::SkinResourceCatalog::upload(
          std::move(*uniquePlan.plan), uniqueDevice);
      const std::size_t uploadComparisons =
          skin::skinResourceRegionIdentityChecksForTesting();
      skin::resetSkinResourceRegionLookupComparisonsForTesting();
      const auto *lastMapping = uniqueUpload.catalog
          ? uniqueUpload.catalog->findResolvedRegion(1,
                                                     uniqueFrames.frames.back())
          : nullptr;
      const std::size_t lookupComparisons =
          skin::skinResourceRegionLookupComparisonsForTesting();
      expect(uniqueUpload.catalog && lastMapping != nullptr &&
                 uniqueUpload.catalog->find(1)->regions.size() == uniqueCount &&
                 uniqueUpload.catalog->find(2)->regions.size() == uniqueCount &&
                 uploadComparisons > uniqueCount * 2 &&
                 uploadComparisons <= uniqueCount * 80 &&
                 lookupComparisons > 0 && lookupComparisons <= 40,
             "primary and alias preflight preserve authored order while immutable lookup uses measured logarithmic comparisons");
    }
  }
  {
    auto conflictingMappingPlan = copyUploadPlan();
    conflictingMappingPlan.images.resize(1);
    conflictingMappingPlan.atlases.clear();
    auto &image = conflictingMappingPlan.images.front();
    image.aliases.clear();
    image.aliasRegions.clear();
    image.aliasRegionMappings.clear();
    const auto conflicting = skin::SkinResolvedRegion{
        .authored = image.regionMappings.front().authored,
        .resolved = {.x=10, .y=0, .w=10, .h=10,
                     .gridColumn=0, .gridRow=0, .gridColumns=1, .gridRows=1}};
    image.regionMappings.push_back(conflicting);
    image.regions.push_back(conflicting.resolved);
    conflictingMappingPlan.decodedBytes = image.pixels.byteSize();
    auto conflictingDevice = std::make_shared<FakeTextureDevice>();
    const auto conflictingUpload = skin::SkinResourceCatalog::upload(
        std::move(conflictingMappingPlan), conflictingDevice);
    expect(!conflictingUpload.catalog && conflictingDevice->creates == 0 &&
               conflictingDevice->live == 0,
           "conflicting duplicate authored mappings fail complete preflight before a texture create");
  }
  std::mutex decoderMutex;
  std::condition_variable decoderCv;
  bool decoderStarted = false;
  skin::SkinResourcePreparationService gatedService(
      [&](std::span<const std::byte>, std::stop_token stop)
          -> std::optional<image_decode::DecodedImageData> {
        {
          std::lock_guard lock(decoderMutex);
          decoderStarted = true;
        }
        decoderCv.notify_all();
        while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return std::nullopt;
      }, 1);
  std::optional<skin::SkinResourcePlanResult> stoppedPlan;
  std::jthread planningThread([&] {
    stoppedPlan = gatedService.decodeAndPlan({.revision=planned.plan->revision.clone(), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration});
  });
  {
    std::unique_lock lock(decoderMutex);
    expect(decoderCv.wait_for(lock, std::chrono::seconds(2), [&] { return decoderStarted; }),
           "injected resource decoder begins an in-flight planning operation");
  }
  std::jthread firstShutdown([&] { gatedService.shutdown(); });
  std::jthread secondShutdown([&] { gatedService.shutdown(); });
  firstShutdown.join();
  secondShutdown.join();
  planningThread.join();
  expect(stoppedPlan && stoppedPlan->cancelled && !stoppedPlan->plan,
         "concurrent shutdown stops the injectable decoder and prevents plan publication");
  const auto afterShutdown = gatedService.validateResources({.revision=planned.plan->revision.readView(), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(!afterShutdown.valid && !afterShutdown.diagnostics.empty() &&
             afterShutdown.diagnostics.front().code == "skin.resource.service_stopped",
         "the service-owned stop state rejects synchronous validation after shutdown begins");
  skin::SkinResourceUploadPlan rollbackPlan{
      .revision = planned.plan->revision.clone(), .images = planned.plan->images, .atlases = planned.plan->atlases,
      .textAtlasesByObject = planned.plan->textAtlasesByObject,
      .decodedBytes = planned.plan->decodedBytes};
  auto counters = std::make_shared<skin::SkinLiveResourceCounters>();
  const auto baselineCounters = counters->snapshot();
  auto failingDevice = std::make_shared<FakeTextureDevice>();
  failingDevice->failAt = 2;
  const auto failedUpload = skin::SkinResourceCatalog::upload(
      std::move(rollbackPlan), failingDevice, counters);
  expect(!failedUpload.catalog && failingDevice->creates == 2 &&
             failingDevice->destroys == 1 && failingDevice->live == 0 &&
             counters->snapshot() == baselineCounters,
         "a partial texture upload rolls back every prior unique handle and its counters exactly once");
  std::stop_source cancelled;
  cancelled.request_stop();
  const auto cancelledPlan = service.decodeAndPlan({.revision=planned.plan->revision.clone(), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration, .stop=cancelled.get_token()});
  expect(cancelledPlan.cancelled && !cancelledPlan.plan && cancelledPlan.diagnostics.empty(),
         "a stop request returns cancellation without publishing a partial plan");
  leasedFs.fileSystem.reset();
  auto device = std::make_shared<FakeTextureDevice>();
  auto uploaded = skin::SkinResourceCatalog::upload(
      std::move(*planned.plan), device, counters);
  planned.plan.reset();
  const auto *uploadedBitmap =
      uploaded.catalog ? uploaded.catalog->findTextAtlasForObject(10) : nullptr;
  const auto *uploadedDistance =
      uploaded.catalog ? uploaded.catalog->findTextAtlasForObject(11) : nullptr;
  expect(uploaded.catalog && device->creates == 7 && device->live == 7 &&
             uploaded.catalog->find(1) && uploaded.catalog->find(2) && uploaded.catalog->find(3) &&
             uploaded.catalog->findTextAtlas(1) && uploaded.catalog->findTextAtlas(2) && uploaded.catalog->findTextAtlas(3) && uploaded.catalog->findTextAtlas(4) &&
             uploaded.catalog->findTextAtlasForObject(6) &&
             uploaded.catalog->findTextAtlasForObject(7) &&
             uploaded.catalog->findTextAtlasForObject(8) &&
             uploaded.catalog->findTextAtlasForObject(9) &&
             uploaded.catalog->findTextAtlasForObject(10) &&
             uploaded.catalog->findTextAtlasForObject(11) &&
             uploaded.catalog->findTextAtlasForObject(12) &&
             uploaded.catalog->findTextAtlasForObject(13) &&
             !uploaded.catalog->findTextAtlasForObject(5) &&
             firstAtlasKey && uploaded.catalog->findTextAtlas(*firstAtlasKey) &&
             uploaded.catalog->find(1)->regions.front().x == 0 &&
             uploaded.catalog->find(2)->regions.front().x == 2 &&
             uploadedBitmap && uploadedBitmap->pages.size() == 2 &&
             uploadedBitmap->pages[0].available &&
             uploadedBitmap->pages[1].available &&
             uploadedBitmap->pages[0].texture.idx ==
                 uploadedBitmap->pages[1].texture.idx &&
             uploadedDistance && uploadedDistance->pages.size() == 2 &&
             uploadedDistance->pages[0].texture.idx ==
                 uploadedBitmap->pages[0].texture.idx &&
             counters->snapshot() ==
                 skin::SkinLiveResourceSnapshot{.liveTextures = 7,
                                                 .liveResources = 1},
         "upload aliases repeated bitmap pages across font types while "
         "preserving per-resource glyph page selection");
  if (!uploaded.catalog) return;
  const skin::SkinSourceRect firstAuthored{.x=0,.y=0,.w=40,.h=20,.gridColumns=4,.gridRows=2};
  const skin::SkinSourceRect aliasAuthored{.x=2,.y=3,.w=20,.h=10,.gridColumns=2,.gridRows=1};
  const auto *firstMapped = uploaded.catalog->findResolvedRegion(1, firstAuthored);
  const auto *aliasMapped = uploaded.catalog->findResolvedRegion(2, aliasAuthored);
  expect(firstMapped && aliasMapped && firstMapped->resolved.x == 0 && firstMapped->resolved.w == 10 &&
             aliasMapped->resolved.x == 2 && aliasMapped->resolved.w == 10,
         "immutable resource lookup preserves each authored frame identity through resolution and alias reuse");
  uploaded.catalog->enterRenderPhase();
  const auto makeBuiltinPixels = [](int width, int height,
                                    unsigned char value) {
    return image_decode::DecodedImageData{
        .width = width,
        .height = height,
        .rgba = std::make_shared<std::vector<unsigned char>>(
            static_cast<std::size_t>(width) *
                static_cast<std::size_t>(height) * 4U,
            value)};
  };
  const int builtinCreatesBefore = device->creates;
  const int builtinDestroysBefore = device->destroys;
  expect(uploaded.catalog->replaceBuiltinImage(
             100, makeBuiltinPixels(2, 2, 0x11U)) &&
             uploaded.catalog->builtinImageResource(100).has_value() &&
             uploaded.catalog->find(
                 *uploaded.catalog->builtinImageResource(100))->width == 2 &&
             uploaded.catalog->replaceBuiltinImage(
                 100, makeBuiltinPixels(2, 2, 0x22U)) &&
             uploaded.catalog->replaceBuiltinImage(
                 100, makeBuiltinPixels(4, 2, 0x33U)) &&
             uploaded.catalog->find(
                 *uploaded.catalog->builtinImageResource(100))->width == 4 &&
             uploaded.catalog->replaceBuiltinImage(100, std::nullopt) &&
             !uploaded.catalog->builtinImageResource(100).has_value() &&
             device->creates == builtinCreatesBefore + 2 &&
             device->updates == 1 &&
             device->destroys == builtinDestroysBefore + 2,
         "selected chart resources update in place when possible, replace "
         "only for size changes, and disappear when the source has none");
  const skin::SkinGeneratedTextureKey generatedKey{
      .sourceObject = 71,
      .authoredOrdinal = 19,
      .layer = skin::SkinGeneratedTextureLayer::Shape};
  const auto firstPixels =
      std::make_shared<const std::vector<std::uint8_t>>(16U, 0x11U);
  const auto equalPixels =
      std::make_shared<const std::vector<std::uint8_t>>(16U, 0x11U);
  const auto changedPixels =
      std::make_shared<std::vector<std::uint8_t>>(16U, 0x22U);
  const auto resizedPixels =
      std::make_shared<const std::vector<std::uint8_t>>(32U, 0x33U);
  const int generatedCreatesBefore = device->creates;
  const int generatedDestroysBefore = device->destroys;
  const int generatedUpdatesBefore = device->updates;
  const auto *createdGenerated = uploaded.catalog->prepareGeneratedTexture(
      generatedKey,
      {.width = 2, .height = 2, .rgba = firstPixels, .contentRevision = 1});
  const auto *cachedGenerated = uploaded.catalog->prepareGeneratedTexture(
      generatedKey,
      {.width = 2, .height = 2, .rgba = equalPixels, .contentRevision = 1});
  const auto *updatedGenerated = uploaded.catalog->prepareGeneratedTexture(
      generatedKey,
      {.width = 2, .height = 2, .rgba = changedPixels, .contentRevision = 2});
  (*changedPixels)[0] = 0x44U;
  const auto *updatedSameBuffer = uploaded.catalog->prepareGeneratedTexture(
      generatedKey,
      {.width = 2, .height = 2, .rgba = changedPixels, .contentRevision = 3});
  const auto *resizedGenerated = uploaded.catalog->prepareGeneratedTexture(
      generatedKey,
      {.width = 4, .height = 2, .rgba = resizedPixels, .contentRevision = 4});
  expect(createdGenerated != nullptr && cachedGenerated != nullptr &&
             updatedGenerated != nullptr && updatedSameBuffer != nullptr &&
             resizedGenerated != nullptr &&
             createdGenerated->key == generatedKey &&
             resizedGenerated->width == 4 && resizedGenerated->height == 2 &&
             device->creates == generatedCreatesBefore + 2 &&
             device->updates == generatedUpdatesBefore + 2 &&
             device->destroys == generatedDestroysBefore + 1,
         "session-generated textures create once, skip a stable revision, "
         "update every dirty revision even when a Pixmap buffer is reused, "
         "and replace only on dimension changes");
  const auto onePixel =
      std::make_shared<const std::vector<std::uint8_t>>(4U, 0xffU);
  const int boundedCreatesBefore = device->creates;
  std::size_t acceptedGenerated = 0;
  for (std::size_t index = 1;
       index < skin::SkinResourcePolicy::maximumGeneratedTextures; ++index) {
    if (uploaded.catalog->prepareGeneratedTexture(
            {.sourceObject = static_cast<skin::SkinObjectId>(100 + index),
             .authoredOrdinal = static_cast<std::uint32_t>(index),
             .layer = skin::SkinGeneratedTextureLayer::Primary},
            {.width = 1, .height = 1, .rgba = onePixel}) != nullptr) {
      ++acceptedGenerated;
    }
  }
  const auto *overLimit = uploaded.catalog->prepareGeneratedTexture(
      {.sourceObject = 9999,
       .authoredOrdinal = 9999,
       .layer = skin::SkinGeneratedTextureLayer::Primary},
      {.width = 1, .height = 1, .rgba = onePixel});
  expect(acceptedGenerated ==
                 skin::SkinResourcePolicy::maximumGeneratedTextures - 1U &&
             overLimit == nullptr &&
             device->creates ==
                 boundedCreatesBefore +
                     static_cast<int>(acceptedGenerated),
         "generated texture count stops exactly at the fixed session budget "
         "without attempting an over-limit upload");
  const int readsBefore = device->creates;
  for (int frame = 0; frame != 120; ++frame) {
    (void)uploaded.catalog->find(1);
    (void)uploaded.catalog->find(2);
    (void)uploaded.catalog->find(9999);
    (void)uploaded.catalog->findTextAtlas(1);
    (void)uploaded.catalog->findTextAtlasForObject(6);
  }
  expect(device->creates == readsBefore,
         "render-phase ID lookups do not upload or access resource files");
  std::weak_ptr<skin::SkinTextureDevice> backendLifetime = device;
  device.reset();
  expect(!backendLifetime.expired(),
         "catalog structurally retains the texture backend through its owner-thread teardown");
  uploaded.catalog.reset();
  expect(backendLifetime.expired(),
         "catalog teardown releases the backend after once-only destruction");
  expect(counters->snapshot() == baselineCounters,
         "catalog teardown returns global ownership counters to the baseline");
  expect(!weak.hasLiveLease(),
         "catalog teardown releases the revision lease");
}
}

void testBitmapFontPagesAreCachedAcrossDecodeRuns() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path source =
      temporary.root / "visible" / "CachedFontPagesFixture";
  const fs::path resources = source / "entry/resources";
  fs::create_directories(resources);
  std::ofstream(source / "entry/play.luaskin") << "return {}\n";

  fs::copy_file(
      fs::path(ASOBMASHOW_SOURCE_DIR) /
          "tests/fixtures/beatoraja_skin/resources/bitmap-font/fixture.fnt",
      resources / "fixture.fnt");
  fs::copy_file(
      fs::path(ASOBMASHOW_SOURCE_DIR) /
          "tests/fixtures/beatoraja_skin/resources/bitmap-font/page.png",
      resources / "page.png");

  const auto package =
      *skin::normalizePackageId("CachedFontPagesFixture").package;
  const auto entry =
      *skin::normalizeEntryPath(package, "entry/play.luaskin").entry;
  skin::SkinStorageRoots roots{
      .visiblePackages = temporary.root / "visible",
      .privateRevisions = temporary.root / "revisions",
      .privateCatalog = temporary.root / "catalog",
      .profileOverlays = temporary.root / "overlays",
      .liveSources = true};
  auto aliases = skin::createPlatformSkinAliasDetector();
  skin::SkinTreeSnapshotter snapshotter(roots, *aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(),
         "cached font pages fixture creates a live revision");
  if (!snapshot.prepared) return;
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease && publishError.empty(),
         "cached font pages fixture publishes a lease");
  if (!lease) return;
  auto leasedFs = skin::LuaSkinFileSystem::create(
      {.revision = lease->readView(), .entry = entry, .storageRoots = roots});
  expect(leasedFs.fileSystem != nullptr,
         "cached font pages fixture creates an entry-aware filesystem");
  if (!leasedFs.fileSystem) return;

  skin::BeatorajaSkinConfiguration configuration;
  const auto model =
      singleFontModel("resources/fixture.fnt", true, "AV\xF0\x9F\x99\x82");
  std::atomic_int fontDecodes = 0;
  skin::SkinResourcePreparationService service(
      [&](std::span<const std::byte> encoded, std::stop_token stop)
          -> std::optional<image_decode::DecodedImageData> {
        if (stop.stop_requested()) return std::nullopt;
        ++fontDecodes;
        return image_decode::decodeImageMemory(
            encoded,
            {.maximumDimension = skin::SkinResourcePolicy::maximumDimension,
             .maximumEncodedBytes =
                 skin::SkinResourcePolicy::maximumEncodedBytes,
             .maximumDecodedBytes = skin::SkinResourcePolicy::maximumImageBytes,
             .stop = stop});
      });
  const std::string revisionKey = lease->revision().lowercaseSha256;

  const auto firstPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = model,
       .configuration = configuration});
  const auto cached = service.decodeCache().entry(revisionKey);
  expect(firstPlan.plan && cached != nullptr && !cached->fontPages.empty() &&
             !cached->pageEncodedBytes.empty(),
         "the first decode run decodes the bitmap font pages and stores them "
         "with their encoded byte sizes in the app-level decode cache");
  if (!firstPlan.plan) return;
  const int decodesAfterFirst = fontDecodes.load();

  const auto secondPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = model,
       .configuration = configuration});
  const auto secondCached = service.decodeCache().entry(revisionKey);
  expect(secondPlan.plan &&
             secondCached != nullptr &&
             secondCached->fontPages.size() == cached->fontPages.size() &&
             secondCached->pageEncodedBytes.size() ==
                 cached->pageEncodedBytes.size() &&
             fontDecodes.load() == decodesAfterFirst &&
             secondPlan.plan->atlases.size() == firstPlan.plan->atlases.size() &&
             secondPlan.plan->atlases.front().glyphs.size() ==
                 firstPlan.plan->atlases.front().glyphs.size() &&
             secondPlan.plan->atlases.front().pages.size() ==
                 firstPlan.plan->atlases.front().pages.size() &&
             secondPlan.plan->atlases.front().key ==
                 firstPlan.plan->atlases.front().key,
         "the second decode run reuses the cached font pages without "
         "re-decoding and produces the identical font atlas");
}

void testBitmapFontCachedPagesChargeEncodedBudgetConsistently() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path source =
      temporary.root / "visible" / "CachedBudgetConsistencyFixture";
  const fs::path resources = source / "entry/resources";
  fs::create_directories(resources);
  std::ofstream(source / "entry/play.luaskin") << "return {}\n";

  fs::copy_file(
      fs::path(ASOBMASHOW_SOURCE_DIR) /
          "tests/fixtures/beatoraja_skin/resources/bitmap-font/fixture.fnt",
      resources / "fixture.fnt");
  fs::copy_file(
      fs::path(ASOBMASHOW_SOURCE_DIR) /
          "tests/fixtures/beatoraja_skin/resources/bitmap-font/page.png",
      resources / "page.png");

  const auto package =
      *skin::normalizePackageId("CachedBudgetConsistencyFixture").package;
  const auto entry =
      *skin::normalizeEntryPath(package, "entry/play.luaskin").entry;
  skin::SkinStorageRoots roots{
      .visiblePackages = temporary.root / "visible",
      .privateRevisions = temporary.root / "revisions",
      .privateCatalog = temporary.root / "catalog",
      .profileOverlays = temporary.root / "overlays",
      .liveSources = true};
  auto aliases = skin::createPlatformSkinAliasDetector();
  skin::SkinTreeSnapshotter snapshotter(roots, *aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(),
         "cached budget fixture creates a live revision");
  if (!snapshot.prepared) return;
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease && publishError.empty(),
         "cached budget fixture publishes a lease");
  if (!lease) return;
  auto leasedFs = skin::LuaSkinFileSystem::create(
      {.revision = lease->readView(), .entry = entry, .storageRoots = roots});
  expect(leasedFs.fileSystem != nullptr,
         "cached budget fixture creates an entry-aware filesystem");
  if (!leasedFs.fileSystem) return;

  skin::BeatorajaSkinConfiguration configuration;
  const auto model =
      singleFontModel("resources/fixture.fnt", true, "AV\xF0\x9F\x99\x82");
  std::atomic_int fontDecodes = 0;
  skin::SkinResourcePreparationService service(
      [&](std::span<const std::byte> encoded, std::stop_token stop)
          -> std::optional<image_decode::DecodedImageData> {
        if (stop.stop_requested()) return std::nullopt;
        ++fontDecodes;
        return image_decode::decodeImageMemory(
            encoded,
            {.maximumDimension = skin::SkinResourcePolicy::maximumDimension,
             .maximumEncodedBytes =
                 skin::SkinResourcePolicy::maximumEncodedBytes,
             .maximumDecodedBytes = skin::SkinResourcePolicy::maximumImageBytes,
             .stop = stop});
      });
  const std::string revisionKey = lease->revision().lowercaseSha256;

  const std::size_t descriptorBytes = fs::file_size(resources / "fixture.fnt");
  const std::size_t pageBytes = fs::file_size(resources / "page.png");
  // Budget admits the descriptor but rejects the page bytes, so the face must
  // be rejected with skin.resource.encoded_limit on every load.
  skin::setSkinResourceAccountingLimitsForTesting(
      descriptorBytes + pageBytes - 1U, /*maximumAtlasSessionBytes=*/3'200);
  struct ResetAccountingLimits {
    ~ResetAccountingLimits() {
      skin::resetSkinResourceAccountingLimitsForTesting();
    }
  } resetAccountingLimits;

  const auto firstPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = model,
       .configuration = configuration});
  expect(!firstPlan.plan,
         "a cold run with an insufficient encoded budget rejects the face");
  const auto cached = service.decodeCache().entry(revisionKey);
  expect(cached == nullptr || cached->fontPages.empty(),
         "a face rejected on the encoded budget must not populate the "
         "app-level decode cache");

  const auto secondPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = model,
       .configuration = configuration});
  expect(!secondPlan.plan,
         "a warm run rejects the face exactly as the cold run did, because the "
         "cached page would have been charged the same encoded bytes");
  const auto secondCached = service.decodeCache().entry(revisionKey);
  expect(secondCached == nullptr || secondCached->fontPages.empty(),
         "the warm rejection also leaves the decode cache empty");
}

void testSkinImagesAreCachedAcrossDecodeRuns() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path source =
      temporary.root / "visible" / "CachedSkinImagesFixture";
  const fs::path resources = source / "entry/resources";
  fs::create_directories(resources);
  std::ofstream(source / "entry/play.luaskin") << "return {}\n";

  fs::copy_file(
      fs::path(ASOBMASHOW_SOURCE_DIR) /
          "tests/fixtures/beatoraja_skin/resources/fixture.png",
      resources / "fixture.png");

  const auto package =
      *skin::normalizePackageId("CachedSkinImagesFixture").package;
  const auto entry =
      *skin::normalizeEntryPath(package, "entry/play.luaskin").entry;
  skin::SkinStorageRoots roots{
      .visiblePackages = temporary.root / "visible",
      .privateRevisions = temporary.root / "revisions",
      .privateCatalog = temporary.root / "catalog",
      .profileOverlays = temporary.root / "overlays",
      .liveSources = true};
  auto aliases = skin::createPlatformSkinAliasDetector();
  skin::SkinTreeSnapshotter snapshotter(roots, *aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(),
         "cached skin images fixture creates a live revision");
  if (!snapshot.prepared) return;
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease && publishError.empty(),
         "cached skin images fixture publishes a lease");
  if (!lease) return;
  auto leasedFs = skin::LuaSkinFileSystem::create(
      {.revision = lease->readView(), .entry = entry, .storageRoots = roots});
  expect(leasedFs.fileSystem != nullptr,
         "cached skin images fixture creates an entry-aware filesystem");
  if (!leasedFs.fileSystem) return;

  skin::BeatorajaSkinConfiguration configuration;
  const auto model = singleImageModel("resources/fixture.png");
  std::atomic_int imageDecodes = 0;
  skin::SkinResourcePreparationService service(
      [&](std::span<const std::byte> encoded, std::stop_token stop)
          -> std::optional<image_decode::DecodedImageData> {
        if (stop.stop_requested()) return std::nullopt;
        ++imageDecodes;
        return image_decode::decodeImageMemory(
            encoded,
            {.maximumDimension = skin::SkinResourcePolicy::maximumDimension,
             .maximumEncodedBytes =
                 skin::SkinResourcePolicy::maximumEncodedBytes,
             .maximumDecodedBytes = skin::SkinResourcePolicy::maximumImageBytes,
             .stop = stop});
      });
  const std::string revisionKey = lease->revision().lowercaseSha256;

  skin::resetSkinImageAppCacheHitsForTesting();
  const auto firstPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = model,
       .configuration = configuration});
  const auto cached = service.decodeCache().entry(revisionKey);
  expect(firstPlan.plan && cached != nullptr && !cached->skinImages.empty(),
         "the first decode run decodes the skin image and stores it in the "
         "app-level decode cache");
  if (!firstPlan.plan) return;
  const int decodesAfterFirst = imageDecodes.load();

  const auto secondPlan = service.decodeAndPlan(
      {.revision = lease->clone(),
       .entry = entry,
       .fileSystem = *leasedFs.fileSystem,
       .model = model,
       .configuration = configuration});
  expect(secondPlan.plan &&
             imageDecodes.load() == decodesAfterFirst &&
             skin::skinImageAppCacheHitsForTesting() >= 1 &&
             secondPlan.plan->images.size() == firstPlan.plan->images.size() &&
             secondPlan.plan->images.front().pixels.width ==
                 firstPlan.plan->images.front().pixels.width &&
             secondPlan.plan->images.front().pixels.height ==
                 firstPlan.plan->images.front().pixels.height &&
             secondPlan.plan->decodedBytes == firstPlan.plan->decodedBytes,
         "the second decode run reuses the cached image without "
         "re-decoding and charges the same decoded budget as the cold run");
}

int main() {
  testBitmapFontDescriptorParsingMatchesPinnedSources();
  testInstalledSelectorBmFontsWhenRequested();
  testBitmapFontsKeepSourceValidMetricsPagesAndMissingGlyphs();
  testBoundedPngAndJpegDecodeBeforeAllocation();
  testSharedSdlTtfRuntimeFinalRelease();
  testSpriteBoundsAndNormalizedGridCells();
  testTextAtlasKeyRejectsNegativePaintExtents();
  testScalableFontOutlineWorkIsBounded();
  testSharedSessionAccountingRejectsDistributedAggregateOverages();
  testChartBuiltinReaderOwnsBytesAndAccountingTransaction();
  testBitmapFontEncodedAccountingCommitsWithAtlasTransaction();
  testSecurePreparationLeaseAliasAndCatalogLifetime();
  testBitmapFontPagesAreCachedAcrossDecodeRuns();
  testBitmapFontCachedPagesChargeEncodedBudgetConsistently();
  testSkinImagesAreCachedAcrossDecodeRuns();
  if (failures) return 1;
  std::cout << "Skin resource catalog tests passed\n";
  return 0;
}
