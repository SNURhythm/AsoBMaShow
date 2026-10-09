#include "TextView.h"
#include "SdlTtfRuntime.h"
#include "FontCacheSession.h"
#include "../RAII.h"
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <array>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/UniformCache.h"
#include "../targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif
#include "bgfx/defines.h"
#include "bx/math.h"
#include <algorithm>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace {
std::mutex g_fontCacheMutex;
constexpr float kMarqueePixelsPerSecond = 48.0f;
constexpr Uint64 kMarqueeStartDelayMs = 700;
constexpr Uint64 kMarqueeEdgeDelayMs = 850;
constexpr int kTextRasterScale = 2;
constexpr std::string_view kReplacementUtf8 = "\xEF\xBF\xBD";

struct CachedFont {
  TTF_Font *font = nullptr;
  int refCount = 0;
  std::uint64_t lastUnused = 0;
};

using FontCacheKey = std::tuple<std::string, int, int>;
// SDL3_ttf fonts stay on their creating thread. Each UI thread owns its cache.
// Borrowed tuple lookups avoid allocating a key while releasing a font.
thread_local std::map<FontCacheKey, CachedFont, std::less<>> g_fontCache;
thread_local std::uint64_t g_fontOpens = 0;
thread_local unsigned g_fontCacheSessions = 0;
thread_local std::uint64_t g_fontCacheUseSerial = 0;
constexpr std::size_t kMaxIdleFonts = 8;
constexpr int kMaxRetainedRasterSize = 128;

// Called with both the SDL_ttf operation guard and cache mutex held. Scanning
// the small cache avoids allocating while releasing views or rolling back.
void trimIdleFonts(std::size_t limit) {
  for (;;) {
    std::size_t idle = 0;
    auto oldest = g_fontCache.end();
    for (auto it = g_fontCache.begin(); it != g_fontCache.end(); ++it) {
      if (it->second.refCount != 0) continue;
      ++idle;
      if (oldest == g_fontCache.end() ||
          it->second.lastUnused < oldest->second.lastUnused) oldest = it;
    }
    if (idle <= limit) return;
    TTF_CloseFont(oldest->second.font);
    g_fontCache.erase(oldest);
  }
}

struct Utf8Token {
  Uint32 codepoint = 0;
  std::string bytes;
};

struct TextLineMetrics {
  int ascent = 0;
  int descent = 0;
  int height = 0;
};

using SurfacePtr = UniqueResource<SDL_Surface, SDL_DestroySurface>;

void addUniquePath(std::vector<std::string> &paths, std::string path) {
  if (path.empty()) {
    return;
  }
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
    paths.push_back(std::move(path));
  }
}

bool canReadFile(const std::string &path) {
  UniqueResource<SDL_IOStream, SDL_CloseIO> rw(SDL_IOFromFile(path.c_str(), "rb"));
  if (rw == nullptr) {
    return false;
  }
  return true;
}

std::vector<std::string> systemFontFallbackPaths() {
  std::vector<std::string> paths;
#if TARGET_OS_OSX
  addUniquePath(paths, "/System/Library/Fonts/SFNS.ttf");
  addUniquePath(paths, "/System/Library/Fonts/Core/SFNS.ttf");
  addUniquePath(paths, "/System/Library/Fonts/CoreUI/SFUI.ttf");
  addUniquePath(paths, "/System/Library/Fonts/AppleSDGothicNeo.ttc");
  addUniquePath(paths,
                "/System/Library/Fonts/LanguageSupport/AppleSDGothicNeo.ttc");
  addUniquePath(paths, "/System/Library/Fonts/Apple Symbols.ttf");
  addUniquePath(paths, "/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
  addUniquePath(paths, "/System/Library/Fonts/Apple Color Emoji.ttc");
  addUniquePath(paths, "/System/Library/Fonts/LastResort.otf");
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
  // iOS system font files are not app-readable; CoreText fallback is used
  // instead when bundled SDL_ttf fonts cannot render a glyph.
#elif defined(_WIN32)
  addUniquePath(paths, "C:/Windows/Fonts/segoeui.ttf");
  addUniquePath(paths, "C:/Windows/Fonts/malgun.ttf");
  addUniquePath(paths, "C:/Windows/Fonts/seguisym.ttf");
  addUniquePath(paths, "C:/Windows/Fonts/seguiemj.ttf");
  addUniquePath(paths, "C:/Windows/Fonts/arialuni.ttf");
#else
  addUniquePath(paths,
                "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
  addUniquePath(paths,
                "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc");
  addUniquePath(paths,
                "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf");
  addUniquePath(paths, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
  addUniquePath(paths, "/usr/share/fonts/truetype/freefont/FreeSans.ttf");
#endif
  return paths;
}

std::string fontPathForWeight(const std::string &path, TextView::FontWeight weight) {
  static constexpr std::string_view notoPaths[] = {
      "assets/fonts/notosanscjkjp.ttf", "assets/fonts/notosansjp.ttf",
      "assets/fonts/notosanskr.otf"};
  for (const auto regular : notoPaths) {
    if (std::string_view(path).ends_with(regular)) {
      return path.substr(0, path.size() - regular.size()) +
          (weight == TextView::FontWeight::Bold
              ? "assets/fonts/notosanscjkjp-bold.otf"
              : "assets/fonts/notosanscjkjp.ttf");
    }
  }
  return path;
}

std::vector<std::string> fontFallbackPaths(const std::string &primaryPath,
                                         TextView::FontWeight weight) {
  std::vector<std::string> paths;
  const auto addFont = [&](const std::string &path) {
    addUniquePath(paths, fontPathForWeight(path, weight));
  };
  addFont(primaryPath);
  addFont("assets/fonts/notosanscjkjp.ttf");
  addFont("assets/fonts/notosanssymbols2.ttf");
  addFont("assets/fonts/arial.ttf");
  for (const auto &path : systemFontFallbackPaths()) {
    addFont(path);
  }
  return paths;
}

int fontStyleForWeight(TextView::FontWeight weight) {
  return weight == TextView::FontWeight::Bold ? TTF_STYLE_BOLD
                                               : TTF_STYLE_NORMAL;
}

TTF_Font *acquireFontCandidate(const std::string &path, int fontSize,
                               int fontStyle, bool required) {
  text_runtime::OperationGuard operation;
  const auto key = std::tie(path, fontSize, fontStyle);
  {
    std::lock_guard<std::mutex> lock(g_fontCacheMutex);
    auto cached = g_fontCache.find(key);
    if (cached != g_fontCache.end()) {
      ++cached->second.refCount;
      return cached->second.font;
    }
  }

  if (!required && !canReadFile(path)) {
    return nullptr;
  }

  UniqueResource<TTF_Font, TTF_CloseFont> opened(TTF_OpenFont(path.c_str(), fontSize));
  if (opened == nullptr && (required || canReadFile(path))) {
    SDL_Log("Failed to load font '%s': %s", path.c_str(), SDL_GetError());
  }
  if (opened != nullptr) {
    ++g_fontOpens;
    TTF_SetFontStyle(opened.get(), fontStyle);
    std::lock_guard<std::mutex> lock(g_fontCacheMutex);
    auto [cached, inserted] = g_fontCache.emplace(key, CachedFont{opened.get(), 1});
    if (!inserted) {
      ++cached->second.refCount;
      return cached->second.font;
    }
  }
  return opened.release();
}

void releaseFontCandidate(const std::string &path, int fontSize, int fontStyle,
                          TTF_Font *font) {
  text_runtime::OperationGuard operation;
  if (font == nullptr) {
    return;
  }

  const auto key = std::tie(path, fontSize, fontStyle);
  std::lock_guard<std::mutex> lock(g_fontCacheMutex);
  auto cached = g_fontCache.find(key);
  if (cached == g_fontCache.end()) {
    TTF_CloseFont(font);
    return;
  }

  --cached->second.refCount;
  if (cached->second.refCount <= 0) {
    if (g_fontCacheSessions > 0 && fontSize <= kMaxRetainedRasterSize) {
      cached->second.lastUnused = ++g_fontCacheUseSerial;
      trimIdleFonts(kMaxIdleFonts);
    } else {
      TTF_CloseFont(cached->second.font);
      g_fontCache.erase(cached);
    }
  }
}

bool decodeNextUtf8(const std::string &text, size_t &index, Utf8Token &token) {
  if (index >= text.size()) {
    return false;
  }

  const size_t start = index;
  const auto first = static_cast<unsigned char>(text[index]);
  Uint32 codepoint = 0;
  size_t length = 0;
  Uint32 minimum = 0;

  if (first < 0x80) {
    codepoint = first;
    length = 1;
  } else if ((first & 0xE0) == 0xC0) {
    codepoint = first & 0x1F;
    length = 2;
    minimum = 0x80;
  } else if ((first & 0xF0) == 0xE0) {
    codepoint = first & 0x0F;
    length = 3;
    minimum = 0x800;
  } else if ((first & 0xF8) == 0xF0) {
    codepoint = first & 0x07;
    length = 4;
    minimum = 0x10000;
  } else {
    token = {0xFFFD, std::string(kReplacementUtf8)};
    ++index;
    return true;
  }

  if (start + length > text.size()) {
    token = {0xFFFD, std::string(kReplacementUtf8)};
    ++index;
    return true;
  }

  for (size_t offset = 1; offset < length; ++offset) {
    const auto byte = static_cast<unsigned char>(text[start + offset]);
    if ((byte & 0xC0) != 0x80) {
      token = {0xFFFD, std::string(kReplacementUtf8)};
      ++index;
      return true;
    }
    codepoint = (codepoint << 6) | (byte & 0x3F);
  }

  if ((length > 1 && codepoint < minimum) || codepoint > 0x10FFFF ||
      (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    token = {0xFFFD, std::string(kReplacementUtf8)};
    ++index;
    return true;
  }

  token = {codepoint, text.substr(start, length)};
  index = start + length;
  return true;
}

bool isExplicitLineBreak(Uint32 codepoint) {
  return codepoint == '\n' || codepoint == '\r';
}

bool isBreakableSpace(Uint32 codepoint) {
  return codepoint == ' ' || codepoint == '\t' || codepoint == 0x3000;
}

bool isIgnorableUnsupportedCodepoint(Uint32 codepoint) {
  return codepoint == 0x200B || codepoint == 0x200C || codepoint == 0x200D ||
         codepoint == 0xFE0E || codepoint == 0xFE0F ||
         (codepoint >= 0xE0100 && codepoint <= 0xE01EF) ||
         (codepoint >= 0x0300 && codepoint <= 0x036F) ||
         (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||
         (codepoint >= 0x1DC0 && codepoint <= 0x1DFF) ||
         (codepoint >= 0x20D0 && codepoint <= 0x20FF) ||
         (codepoint >= 0xFE20 && codepoint <= 0xFE2F);
}

struct RasterTextSize {
  int width = 0;
  int height = 0;
};

RasterTextSize sizeUtf8(TTF_Font *font, const std::string &utf8) {
  text_runtime::OperationGuard operation;
  if (font == nullptr || utf8.empty()) {
    return {};
  }

  RasterTextSize size;
  if (!TTF_GetStringSize(font, utf8.c_str(), 0, &size.width, &size.height)) {
    return {};
  }
  return size;
}

int rasterFontSizeFor(int logicalFontSize) {
  return std::max(1, logicalFontSize * kTextRasterScale);
}

int rasterLengthFor(int logicalLength) {
  return std::max(0, logicalLength * kTextRasterScale);
}

int logicalLengthFor(int rasterLength) {
  return std::max(0, (rasterLength + kTextRasterScale - 1) /
                         kTextRasterScale);
}

} // namespace

text_runtime::FontCacheSession::FontCacheSession() noexcept
    : initialized_(text_runtime::acquire()) {
  if (!initialized_) return;
  OperationGuard operation;
  std::lock_guard lock(g_fontCacheMutex);
  ++g_fontCacheSessions;
}

text_runtime::FontCacheSession::~FontCacheSession() {
  if (!initialized_) return;
  {
    OperationGuard operation;
    std::lock_guard lock(g_fontCacheMutex);
    if (--g_fontCacheSessions == 0) trimIdleFonts(0);
  }
  // release takes the lifecycle lock before the operation lock.
  text_runtime::release();
}

text_runtime::FontCacheStats text_runtime::fontCacheStatsForTesting() {
  OperationGuard operation;
  std::lock_guard lock(g_fontCacheMutex);
  FontCacheStats stats;
  stats.opens = g_fontOpens;
  for (const auto &[key, cached] : g_fontCache) {
    if (cached.refCount > 0) ++stats.active;
    else ++stats.idle;
  }
  return stats;
}

TextView::TextView(const std::string &fontPath, int fontSize,
                   FontWeight fontWeight)
    : View(), texture(BGFX_INVALID_HANDLE) {
  this->fontSize = fontSize;
  fontWeight_ = fontWeight;
  fontStyle_ = fontStyleForWeight(fontWeight);
  this->fontRasterSize = rasterFontSizeFor(fontSize);
  primaryFontPath_ = fontPathForWeight(fontPath, fontWeight);
  fallbackFontPaths = fontFallbackPaths(fontPath, fontWeight);
  ttfInitialized = text_runtime::acquire();
  auto rollback = makeScopeExit([this] { releaseFontResources(); });
  if (ttfInitialized) {
    while (nextFallbackFontPath < fallbackFontPaths.size()) {
      const bool required = nextFallbackFontPath == 0;
      TTF_Font *opened = loadFallbackFontAt(nextFallbackFontPath, required);
      ++nextFallbackFontPath;
      if (opened == nullptr) {
        continue;
      }
      break;
    }

    if (font == nullptr) {
      SDL_Log("Failed to load any font for primary path '%s'",
              fontPath.c_str());
    }
  }
  color = {255, 255, 255, 255}; // Default color: white
  rect = {0, 0, 0, 0};
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
  YGNodeSetMeasureFunc(getNode(), measureFunc);
  rollback.dismiss();
}

TextView::~TextView() {
  if (bgfx::isValid(texture)) {
    bgfx::destroy(texture);
  }
  releaseFontResources();
}

void TextView::releaseFontResources() {
  for (auto &face : fontFaces) {
    if (face.font != nullptr) {
      releaseFontCandidate(face.path, fontRasterSize, fontStyle_, face.font);
      face.font = nullptr;
    }
  }
  font = nullptr;
  if (ttfInitialized) {
    text_runtime::release();
    ttfInitialized = false;
  }
}

void TextView::setText(const std::string &newText) {
  localizedText_ = i18n::Text("");
  setResolvedText(newText);
}

void TextView::setLocalizedText(const i18n::Text &newText) {
  localizedText_ = newText;
  setResolvedText(localizedText_.resolve());
}

void TextView::onLanguageChanged() {
  View::onLanguageChanged();
  if (localizedText_.isLocalized()) {
    setResolvedText(localizedText_.resolve());
  }
}

void TextView::setResolvedText(const std::string &newText) {
  if (newText == text) {
    return;
  }
  this->text = newText;
  marqueeStartedAt = SDL_GetTicks();
  metricsDirty = true;
  invalidateTexture();
  updateTextMetrics();
  if (!deferTextureMaterialization) {
    createTexture();
  }
}

void TextView::setDeferredTextureMaterialization(bool deferred) {
  if (deferTextureMaterialization == deferred) {
    return;
  }
  deferTextureMaterialization = deferred;
  if (!deferTextureMaterialization && !bgfx::isValid(texture)) {
    createTexture();
  }
}

void TextView::renderImpl(RenderContext &context) {
  if (!bgfx::isValid(texture)) {
    createTexture();
  }
  if (!bgfx::isValid(texture)) {
    return;
  }

  SDL_Rect drawRect = resolvedTextRect();
  if (drawRect.w <= 0 || drawRect.h <= 0) {
    return;
  }
  const float rotationDegrees = getRotationDegrees();
  const bool clip =
      overflow != TextOverflow::Visible && getContentWidth() > 0 &&
      getContentHeight() > 0;
  if (rotationDegrees == 0.0f && overflow == TextOverflow::Marquee &&
      !wrapEnabled && !textFitBounds().has_value() &&
      rect.w > getContentWidth()) {
    drawRect.x = getContentX() - static_cast<int>(
                                   std::round(marqueeOffset(getContentWidth())));
  }

  const auto submitText = [this, &context, &drawRect]() {
    const float left = static_cast<float>(drawRect.x);
    const float top = static_cast<float>(drawRect.y);
    const float right = left + static_cast<float>(drawRect.w);
    const float bottom = top + static_cast<float>(drawRect.h);
    const std::array vertices = {
        rendering::PosTexCoord0Vertex{left, top, 0.0f, 0.0f, 0.0f},
        rendering::PosTexCoord0Vertex{right, top, 0.0f, 1.0f, 0.0f},
        rendering::PosTexCoord0Vertex{right, bottom, 0.0f, 1.0f, 1.0f},
        rendering::PosTexCoord0Vertex{left, bottom, 0.0f, 0.0f, 1.0f},
    };

    constexpr std::array<uint16_t, 6> indices = {0, 1, 2, 0, 2, 3};
    static const bgfx::ProgramHandle kProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
    auto state = context.makeUiBatchState(
        kProgram, BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
    state.texture = texture;
    state.sampler = s_texColor;
    context.appendUiTextured(vertices, indices, state);
  };

  if (clip) {
    ScissorScope scissor(context, getContentX(), getContentY(),
                         getContentWidth(), getContentHeight());
    submitText();
  } else {
    submitText();
  }
}

SDL_Rect TextView::resolvedTextRect() const {
  const int contentHeight = rect.h > 0 ? rect.h : textLineHeight();
  SDL_Rect drawRect = {getContentX(), getContentY(), rect.w, contentHeight};
  int width = getContentWidth();
  int height = getContentHeight();
  if (const auto bounds = textFitBounds()) {
    drawRect.x = static_cast<int>(bounds->x);
    drawRect.y = static_cast<int>(bounds->y);
    width = static_cast<int>(bounds->width);
    height = static_cast<int>(bounds->height);
    if (drawRect.w > 0 && drawRect.h > 0) {
      const float scale = std::min(
          {1.0f, static_cast<float>(width) / drawRect.w,
           static_cast<float>(height) / drawRect.h});
      drawRect.w = static_cast<int>(std::floor(drawRect.w * scale));
      drawRect.h = static_cast<int>(std::floor(drawRect.h * scale));
    }
  }

  switch (align) {
  case TextAlign::LEFT:
    break;
  case TextAlign::CENTER:
    drawRect.x += (width - drawRect.w) / 2;
    break;
  case TextAlign::RIGHT:
    drawRect.x += width - drawRect.w;
    break;
  }

  switch (valign) {
  case TextVAlign::TOP:
    break;
  case TextVAlign::MIDDLE:
    drawRect.y += (height - drawRect.h) / 2;
    break;
  case TextVAlign::BOTTOM:
    drawRect.y += height - drawRect.h;
    break;
  }

  return drawRect;
}

View::RenderBounds TextView::renderingBounds() const {
  const RenderBounds frame = View::renderingBounds();
  const SDL_Rect drawRect = resolvedTextRect();
  // Decorations and input carets still paint when there are no glyphs.
  // Include overflowing text as well as the view's own frame when culling.
  const float left = std::min(frame.x, static_cast<float>(drawRect.x));
  const float top = std::min(frame.y, static_cast<float>(drawRect.y));
  const float right = std::max(frame.x + frame.width,
                               static_cast<float>(drawRect.x + drawRect.w));
  const float bottom = std::max(frame.y + frame.height,
                                static_cast<float>(drawRect.y + drawRect.h));
  return {.x = left, .y = top, .width = right - left, .height = bottom - top};
}

int TextView::textLineHeight() const {
  return logicalLengthFor(rasterTextLineHeight());
}

int TextView::rasterTextLineHeight() const {
  return std::max(fontLineHeight, fontAscent + fontDescent);
}

void TextView::includeFontMetrics(TTF_Font *loadedFont) {
  text_runtime::OperationGuard operation;
  if (loadedFont == nullptr) {
    return;
  }

  fontLineHeight = std::max(fontLineHeight, TTF_GetFontHeight(loadedFont));
  fontLineSkip = std::max(fontLineSkip, TTF_GetFontLineSkip(loadedFont));
  fontAscent = std::max(fontAscent, TTF_GetFontAscent(loadedFont));
  fontDescent = std::max(fontDescent, -TTF_GetFontDescent(loadedFont));
}

void TextView::includeIOSSystemFontMetrics() {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (iosSystemFontMetricsIncluded) {
    return;
  }

  const IOSSystemTextMetrics metrics = GetIOSSystemTextMetrics(fontRasterSize);
  iosSystemFontLineHeight = metrics.height;
  iosSystemFontAscent = metrics.ascent;
  iosSystemFontDescent = metrics.descent;
  fontLineHeight = std::max(fontLineHeight, metrics.height);
  fontAscent = std::max(fontAscent, metrics.ascent);
  fontDescent = std::max(fontDescent, metrics.descent);
  iosSystemFontMetricsIncluded = true;
#endif
}

TTF_Font *TextView::loadFallbackFontAt(size_t pathIndex, bool required) {
  if (pathIndex >= fallbackFontPaths.size()) {
    return nullptr;
  }

  const std::string &path = fallbackFontPaths[pathIndex];
  TTF_Font *opened =
      acquireFontCandidate(path, fontRasterSize, fontStyle_, required);
  if (opened == nullptr) {
    return nullptr;
  }

  auto rollback = makeScopeExit([&] {
    releaseFontCandidate(path, fontRasterSize, fontStyle_, opened);
  });
  fontFaces.push_back({opened, path});
  rollback.dismiss();
  if (font == nullptr) {
    font = opened;
  }
  includeFontMetrics(opened);
  return opened;
}

bool TextView::hasFontSource(const SelectedFont &source) const {
  return source.font != nullptr || source.iosSystemFont;
}

bool TextView::sameFontSource(const SelectedFont &lhs,
                              const SelectedFont &rhs) const {
  return lhs.font == rhs.font && lhs.iosSystemFont == rhs.iosSystemFont;
}

int TextView::measureFontSourceTextWidth(const SelectedFont &source,
                                         const std::string &utf8,
                                         int *rasterHeight) {
  if (rasterHeight != nullptr) *rasterHeight = 0;
  if (utf8.empty()) {
    return 0;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (source.iosSystemFont) {
    includeIOSSystemFontMetrics();
    if (rasterHeight != nullptr) *rasterHeight = iosSystemFontLineHeight;
    return MeasureIOSSystemTextWidth(utf8, fontRasterSize);
  }
#endif

  const RasterTextSize size = sizeUtf8(source.font, utf8);
  if (rasterHeight != nullptr) *rasterHeight = size.height;
  return size.width;
}

int TextView::fontSourceAscent(const SelectedFont &source) {
  text_runtime::OperationGuard operation;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (source.iosSystemFont) {
    includeIOSSystemFontMetrics();
    return iosSystemFontAscent;
  }
#endif

  return source.font == nullptr ? 0 : TTF_GetFontAscent(source.font);
}

SDL_Surface *TextView::renderFontSourceTextSurface(const SelectedFont &source,
                                                   const std::string &utf8) {
  text_runtime::OperationGuard operation;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (source.iosSystemFont) {
    includeIOSSystemFontMetrics();
    return RenderIOSSystemTextSurface(utf8, fontRasterSize, color);
  }
#endif

  if (source.font == nullptr || utf8.empty()) {
    return nullptr;
  }
  return TTF_RenderText_Blended(source.font, utf8.c_str(), 0, color);
}

TextView::SelectedFont TextView::selectFont(Uint32 codepoint) {
  if (fontFaces.empty()) {
    return {};
  }

  auto cached = fontSelectionCache.find(codepoint);
  if (cached != fontSelectionCache.end()) {
    return cached->second;
  }

  // This view and its cached selections belong to the font's creating thread.
  // Cache hits do not call SDL_ttf; guard only glyph lookup and font loading.
  text_runtime::OperationGuard operation;
  for (const auto &face : fontFaces) {
    if (face.font != nullptr && TTF_FontHasGlyph(face.font, codepoint)) {
      SelectedFont source = {face.font, false};
      fontSelectionCache[codepoint] = source;
      return source;
    }
  }

  if (isIgnorableUnsupportedCodepoint(codepoint)) {
    fontSelectionCache[codepoint] = {};
    return {};
  }

  while (nextFallbackFontPath < fallbackFontPaths.size()) {
    TTF_Font *opened = loadFallbackFontAt(nextFallbackFontPath, false);
    ++nextFallbackFontPath;
    if (opened != nullptr && TTF_FontHasGlyph(opened, codepoint)) {
      SelectedFont source = {opened, false};
      fontSelectionCache[codepoint] = source;
      return source;
    }
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  SelectedFont source = {nullptr, true};
  includeIOSSystemFontMetrics();
  fontSelectionCache[codepoint] = source;
  return source;
#else
  SelectedFont source = {fontFaces.empty() ? nullptr : fontFaces.back().font,
                         false};
  fontSelectionCache[codepoint] = source;
  return source;
#endif
}

bool TextView::primaryFontSupportsText(const std::string &utf8) const {
  text_runtime::OperationGuard operation;
  if (font == nullptr) {
    return false;
  }

  size_t index = 0;
  Utf8Token token;
  while (decodeNextUtf8(utf8, index, token)) {
    if (isExplicitLineBreak(token.codepoint)) {
      continue;
    }
    if (!TTF_FontHasGlyph(font, token.codepoint)) {
      return false;
    }
  }
  return true;
}

int TextView::measureTextWidth(const std::string &utf8) {
  return logicalLengthFor(measureRasterTextWidth(utf8));
}

int TextView::measureRasterTextWidth(const std::string &utf8, int *rasterHeight) {
  if (rasterHeight != nullptr) *rasterHeight = 0;
  if (utf8.empty() || fontFaces.empty()) {
    return 0;
  }
  if (rasterHeight != nullptr) {
    ensureFontsForText(utf8);
    *rasterHeight = rasterTextLineHeight();
  }

  int totalWidth = 0;
  SelectedFont runSource;
  std::string runText;
  const auto measureRun = [&]() {
    int runHeight = 0;
    totalWidth += measureFontSourceTextWidth(runSource, runText, &runHeight);
    if (rasterHeight != nullptr) {
      // Font-wide height/descent can exclude ink below the baseline. Match the
      // actual SDL_ttf run surface, including its baseline-alignment offset.
      *rasterHeight = std::max(*rasterHeight,
          fontAscent - fontSourceAscent(runSource) + runHeight);
    }
  };
  size_t index = 0;
  Utf8Token token;
  while (decodeNextUtf8(utf8, index, token)) {
    if (isExplicitLineBreak(token.codepoint)) {
      break;
    }

    SelectedFont tokenSource = selectFont(token.codepoint);
    if (!hasFontSource(tokenSource)) {
      continue;
    }
    if (hasFontSource(runSource) && !sameFontSource(tokenSource, runSource)) {
      measureRun();
      runText.clear();
    }
    runSource = tokenSource;
    runText += token.bytes;
  }

  if (!runText.empty()) {
    measureRun();
  }
  return totalWidth;
}

void TextView::ensureFontsForText(const std::string &utf8) {
  size_t index = 0;
  Utf8Token token;
  while (decodeNextUtf8(utf8, index, token)) {
    if (isExplicitLineBreak(token.codepoint)) {
      continue;
    }
    selectFont(token.codepoint);
  }
}

std::vector<std::string> TextView::wrappedTextLines(int wrapWidth) {
  std::vector<std::string> lines;
  std::string currentLine;
  size_t lastBreak = std::string::npos;

  const auto pushCurrentLine = [&]() {
    lines.push_back(currentLine);
    currentLine.clear();
    lastBreak = std::string::npos;
  };

  size_t index = 0;
  Utf8Token token;
  while (decodeNextUtf8(text, index, token)) {
    if (isExplicitLineBreak(token.codepoint)) {
      pushCurrentLine();
      if (token.codepoint == '\r' && index < text.size() &&
          text[index] == '\n') {
        ++index;
      }
      continue;
    }

    currentLine += token.bytes;
    if (isBreakableSpace(token.codepoint)) {
      lastBreak = currentLine.size();
    }

    if (wrapWidth <= 0) {
      continue;
    }

    const int currentWidth = measureRasterTextWidth(currentLine);
    if (wrapWidth > 0 && currentWidth > wrapWidth && currentLine.size() > 0) {
      if (lastBreak != std::string::npos && lastBreak > 0) {
        std::string nextLine = currentLine.substr(lastBreak);
        while (!nextLine.empty()) {
          Utf8Token leading;
          size_t leadingIndex = 0;
          if (!decodeNextUtf8(nextLine, leadingIndex, leading) ||
              !isBreakableSpace(leading.codepoint)) {
            break;
          }
          nextLine.erase(0, leadingIndex);
        }

        currentLine.resize(lastBreak);
        while (!currentLine.empty()) {
          size_t trimIndex = 0;
          size_t previousIndex = 0;
          Utf8Token trailing;
          while (trimIndex < currentLine.size()) {
            previousIndex = trimIndex;
            decodeNextUtf8(currentLine, trimIndex, trailing);
          }
          if (!isBreakableSpace(trailing.codepoint)) {
            break;
          }
          currentLine.erase(previousIndex);
        }

        lines.push_back(currentLine);
        currentLine = nextLine;
        lastBreak = std::string::npos;
      } else {
        const std::string overflowingToken = token.bytes;
        currentLine.resize(currentLine.size() - overflowingToken.size());
        if (!currentLine.empty()) {
          lines.push_back(currentLine);
        }
        currentLine = overflowingToken;
        lastBreak = std::string::npos;
      }
    }
  }

  if (!currentLine.empty() || lines.empty()) {
    lines.push_back(currentLine);
  }
  return lines;
}

SDL_Surface *TextView::renderFallbackTextSurface(int wrapWidth,
                                                 int &surfaceWidth,
                                                 int &surfaceHeight) {
  surfaceWidth = 0;
  surfaceHeight = 0;
  if (fontFaces.empty() || text.empty()) {
    return nullptr;
  }

  ensureFontsForText(text);

  TextLineMetrics metrics = {fontAscent, fontDescent, rasterTextLineHeight()};
  if (metrics.height <= 0) {
    return nullptr;
  }

  const std::vector<std::string> lines =
      wrapWidth > 0 ? wrappedTextLines(wrapWidth) : wrappedTextLines(0);
  int width = 0;
  int lineHeight = metrics.height;
  for (const auto &line : lines) {
    int measuredHeight = 0;
    width = std::max(width, measureRasterTextWidth(line, &measuredHeight));
    lineHeight = std::max(lineHeight, measuredHeight);
  }

  const int targetWidth = std::max(1, width);
  const int targetHeight =
      std::max(1, lineHeight * static_cast<int>(lines.size()));
  SurfacePtr surface(SDL_CreateSurface(targetWidth, targetHeight, SDL_PIXELFORMAT_BGRA32));
  if (surface == nullptr) {
    SDL_Log("Failed to create text fallback surface: %s", SDL_GetError());
    return nullptr;
  }

  SDL_FillSurfaceRect(surface.get(), nullptr,
               SDL_MapSurfaceRGBA(surface.get(), 0, 0, 0, 0));

  for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
    const std::string &line = lines[lineIndex];
    std::vector<FontRun> runs;
    SelectedFont runSource;
    size_t tokenIndex = 0;
    Utf8Token token;
    while (decodeNextUtf8(line, tokenIndex, token)) {
      SelectedFont tokenSource = selectFont(token.codepoint);
      if (!hasFontSource(tokenSource)) {
        continue;
      }
      if (runs.empty() || !sameFontSource(runSource, tokenSource)) {
        runs.push_back({tokenSource, ""});
        runSource = tokenSource;
      }
      runs.back().text += token.bytes;
    }

    const int spareWidth = targetWidth - measureRasterTextWidth(line);
    int x = align == TextAlign::CENTER ? spareWidth / 2
            : align == TextAlign::RIGHT ? spareWidth : 0;
    const int lineTop = lineHeight * static_cast<int>(lineIndex);
    for (const auto &run : runs) {
      if (!hasFontSource(run.source) || run.text.empty()) {
        continue;
      }

      SurfacePtr runSurface(renderFontSourceTextSurface(run.source, run.text));
      if (runSurface == nullptr) {
        SDL_Log("Failed to render fallback text run: %s", SDL_GetError());
        continue;
      }

      SDL_SetSurfaceBlendMode(runSurface.get(), SDL_BLENDMODE_NONE);
      SDL_Rect dst = {x,
                      lineTop + metrics.ascent - fontSourceAscent(run.source),
                      runSurface->w, runSurface->h};
      SDL_BlitSurface(runSurface.get(), nullptr, surface.get(), &dst);
      x += measureFontSourceTextWidth(run.source, run.text);
    }
  }

  surfaceWidth = width;
  surfaceHeight = targetHeight;
  return surface.release();
}

float TextView::marqueeOffset(int viewportWidth) {
  const int overflowWidth = rect.w - viewportWidth;
  if (overflowWidth <= 0) {
    return 0.0f;
  }

  if (marqueeStartedAt == 0) {
    marqueeStartedAt = SDL_GetTicks();
  }

  const float scrollDurationMs =
      static_cast<float>(overflowWidth) / kMarqueePixelsPerSecond * 1000.0f;
  const Uint64 scrollMs =
      std::max<Uint64>(1, static_cast<Uint64>(std::round(scrollDurationMs)));
  const Uint64 cycleMs = kMarqueeStartDelayMs + scrollMs + kMarqueeEdgeDelayMs +
                         scrollMs + kMarqueeEdgeDelayMs;
  Uint64 phase = (SDL_GetTicks() - marqueeStartedAt) % cycleMs;

  if (phase < kMarqueeStartDelayMs) {
    return 0.0f;
  }
  phase -= kMarqueeStartDelayMs;

  if (phase < scrollMs) {
    return static_cast<float>(overflowWidth) * static_cast<float>(phase) /
           static_cast<float>(scrollMs);
  }
  phase -= scrollMs;

  if (phase < kMarqueeEdgeDelayMs) {
    return static_cast<float>(overflowWidth);
  }
  phase -= kMarqueeEdgeDelayMs;

  if (phase < scrollMs) {
    return static_cast<float>(overflowWidth) *
           (1.0f - static_cast<float>(phase) / static_cast<float>(scrollMs));
  }

  return 0.0f;
}

void TextView::setColor(SDL_Color newColor) {
  themedColorProvider = nullptr;
  if (newColor.r == color.r && newColor.g == color.g && newColor.b == color.b &&
      newColor.a == color.a) {
    return;
  }
  this->color = newColor;
  invalidateTexture();
  if (!deferTextureMaterialization) {
    createTexture();
  }
}

void TextView::setThemedColor(ThemeColorProvider provider) {
  themedColorProvider = std::move(provider);
  if (!themedColorProvider) {
    return;
  }
  const Color themedColor = themedColorProvider();
  SDL_Color newColor{themedColor.r, themedColor.g, themedColor.b,
                     themedColor.a};
  if (newColor.r == color.r && newColor.g == color.g && newColor.b == color.b &&
      newColor.a == color.a) {
    return;
  }
  color = newColor;
  invalidateTexture();
  if (!deferTextureMaterialization) {
    createTexture();
  }
}

void TextView::onThemeChanged() {
  View::onThemeChanged();
  if (!themedColorProvider) {
    return;
  }
  const Color themedColor = themedColorProvider();
  SDL_Color newColor{themedColor.r, themedColor.g, themedColor.b,
                     themedColor.a};
  if (newColor.r == color.r && newColor.g == color.g && newColor.b == color.b &&
      newColor.a == color.a) {
    return;
  }
  color = newColor;
  invalidateTexture();
  if (!deferTextureMaterialization) {
    createTexture();
  }
}

void TextView::invalidateTexture() {
  if (bgfx::isValid(texture)) {
    bgfx::destroy(texture);
  }
  texture = BGFX_INVALID_HANDLE;
}

void TextView::updateTextMetrics(bool markDirty, int requestedWrapWidth) {
  text_runtime::OperationGuard operation;
  const int previousWidth = rect.w;
  const int previousHeight = rect.h;
  const int effectiveWrapWidth =
      wrapEnabled ? std::max(0, requestedWrapWidth >= 0 ? requestedWrapWidth
                                                        : currentWrapWidth)
                  : 0;
  if (!metricsDirty && effectiveWrapWidth == currentWrapWidth) {
    return;
  }

  const bool wrapWidthChanged = effectiveWrapWidth != currentWrapWidth;
  currentWrapWidth = effectiveWrapWidth;
  metricsDirty = false;
  if (wrapWidthChanged) {
    invalidateTexture();
  }

  if (text.empty() || fontFaces.empty()) {
    rect.w = 0;
    rect.h = 0;
    if (markDirty && (rect.w != previousWidth || rect.h != previousHeight)) {
      YGNodeMarkDirty(getNode());
      applyYogaLayoutFromRoot();
    }
    return;
  }
  const int rasterWrapWidth = rasterLengthFor(effectiveWrapWidth);
  // Compose explicit lines and aligned wrapping consistently across font sources.
  const bool usePrimaryFont =
      font != nullptr && text.find_first_of("\r\n") == std::string::npos &&
      (!wrapEnabled || align == TextAlign::LEFT) && primaryFontSupportsText(text);
  int rasterWidth = 0;
  int rasterHeight = 0;
  if (usePrimaryFont) {
    // SDL3 can measure the same wrapping/shaping that its surface renderer uses.
    // Avoid building fallback runs and repeatedly shaping growing prefixes.
    if (wrapEnabled && rasterWrapWidth > 0) {
      TTF_GetStringSizeWrapped(font, text.c_str(), 0, rasterWrapWidth,
                               &rasterWidth, &rasterHeight);
    } else {
      const RasterTextSize size = sizeUtf8(font, text);
      rasterWidth = size.width;
      rasterHeight = size.height;
    }
  } else {
    ensureFontsForText(text);
    const int lineHeightForFonts = rasterTextLineHeight();
    if (lineHeightForFonts <= 0) {
      rect.w = 0;
      rect.h = 0;
      return;
    }
    const auto lines = wrappedTextLines(rasterWrapWidth);
    int lineHeight = lineHeightForFonts;
    for (const auto &line : lines) {
      int measuredHeight = 0;
      rasterWidth = std::max(rasterWidth, measureRasterTextWidth(line, &measuredHeight));
      lineHeight = std::max(lineHeight, measuredHeight);
    }
    rasterHeight =
        lineHeight * static_cast<int>(std::max<std::size_t>(1, lines.size()));
  }
  rect.w = logicalLengthFor(rasterWidth);
  rect.h = logicalLengthFor(rasterHeight);
  if (markDirty && (rect.w != previousWidth || rect.h != previousHeight)) {
    YGNodeMarkDirty(getNode());
    applyYogaLayoutFromRoot();
  }
}

void TextView::createTexture() {
  text_runtime::OperationGuard operation;
  updateTextMetrics(false);
  invalidateTexture();

  if (text.empty() || fontFaces.empty()) {
    return;
  }
  const int rasterWrapWidth = rasterLengthFor(currentWrapWidth);
  SurfacePtr surface(nullptr);
  int fallbackSurfaceWidth = 0;
  int fallbackSurfaceHeight = 0;
  // Compose explicit lines and aligned wrapping consistently across font sources.
  const bool usePrimaryFont =
      font != nullptr && text.find_first_of("\r\n") == std::string::npos &&
      (!wrapEnabled || align == TextAlign::LEFT) && primaryFontSupportsText(text);
  if (usePrimaryFont && wrapEnabled && rasterWrapWidth > 0) {
    surface.reset(TTF_RenderText_Blended_Wrapped(font, text.c_str(), 0, color, rasterWrapWidth));
  } else if (usePrimaryFont) {
    surface.reset(TTF_RenderText_Blended(font, text.c_str(), 0, color));
  } else {
    surface.reset(renderFallbackTextSurface(
        wrapEnabled && rasterWrapWidth > 0 ? rasterWrapWidth : 0,
        fallbackSurfaceWidth, fallbackSurfaceHeight));
  }
  if (surface == nullptr && usePrimaryFont && fontFaces.size() > 1) {
    surface.reset(renderFallbackTextSurface(
        wrapEnabled && rasterWrapWidth > 0 ? rasterWrapWidth : 0,
        fallbackSurfaceWidth, fallbackSurfaceHeight));
  }
  if (!surface) {
    SDL_Log("Failed to render text: %s", SDL_GetError());
    return;
  }
  (void)fallbackSurfaceWidth;
  (void)fallbackSurfaceHeight;
  texture = rendering::sdlSurfaceToBgfxTexture(surface.get());
}

YGSize TextView::measureFunc(YGNodeConstRef node, float width,
                             YGMeasureMode widthMode, float height,
                             YGMeasureMode heightMode) {
  auto *view = static_cast<TextView *>(YGNodeGetContext(node));
  (void)height;
  (void)heightMode;
  if (view->wrapEnabled && widthMode != YGMeasureModeUndefined &&
      width > 0.0f) {
    view->updateTextMetrics(false, static_cast<int>(std::floor(width)));
  }

  float measuredWidth = static_cast<float>(view->rect.w);
  if (view->overflow != TextOverflow::Visible &&
      widthMode != YGMeasureModeUndefined && width > 0.0f) {
    measuredWidth = widthMode == YGMeasureModeExactly
                        ? width
                        : std::min(measuredWidth, width);
  }
  return {measuredWidth, static_cast<float>(view->rect.h)};
}

void TextView::setAlign(TextAlign newAlign) {
  if (align == newAlign) return;
  align = newAlign;
  if (!wrapEnabled && text.find_first_of("\r\n") == std::string::npos) return;
  metricsDirty = true;
  invalidateTexture();
  updateTextMetrics();
  if (!deferTextureMaterialization) createTexture();
}

void TextView::setVAlign(TextVAlign newVAlign) { this->valign = newVAlign; }

void TextView::setOverflow(TextOverflow newOverflow) {
  if (overflow == newOverflow) {
    return;
  }
  overflow = newOverflow;
  marqueeStartedAt = SDL_GetTicks();
  YGNodeMarkDirty(getNode());
  applyYogaLayoutFromRoot();
}

void TextView::setWrap(bool enabled) {
  if (wrapEnabled == enabled) {
    return;
  }
  wrapEnabled = enabled;
  if (!wrapEnabled) {
    currentWrapWidth = 0;
  }
  metricsDirty = true;
  invalidateTexture();
  updateTextMetrics();
  if (!deferTextureMaterialization) {
    createTexture();
  }
}
