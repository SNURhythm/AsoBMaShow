#include "TextView.h"
#include "../RAII.h"
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
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
#include <utility>

namespace {
std::mutex g_ttfMutex;
int g_ttfRefCount = 0;
std::mutex g_fontCacheMutex;
constexpr float kMarqueePixelsPerSecond = 48.0f;
constexpr Uint64 kMarqueeStartDelayMs = 700;
constexpr Uint64 kMarqueeEdgeDelayMs = 850;
constexpr std::string_view kReplacementUtf8 = "\xEF\xBF\xBD";

struct CachedFont {
  TTF_Font *font = nullptr;
  int refCount = 0;
};

std::map<std::string, CachedFont> g_fontCache;

struct Utf8Token {
  Uint32 codepoint = 0;
  std::string bytes;
};

struct TextLineMetrics {
  int ascent = 0;
  int descent = 0;
  int height = 0;
};

using SurfacePtr = UniqueResource<SDL_Surface, SDL_FreeSurface>;

bool acquireTtf() {
  std::lock_guard<std::mutex> lock(g_ttfMutex);
  if (g_ttfRefCount == 0 && TTF_Init() != 0) {
    SDL_Log("Failed to initialize SDL_ttf: %s", TTF_GetError());
    return false;
  }
  ++g_ttfRefCount;
  return true;
}

void releaseTtf() {
  std::lock_guard<std::mutex> lock(g_ttfMutex);
  if (g_ttfRefCount <= 0) {
    return;
  }
  --g_ttfRefCount;
  if (g_ttfRefCount == 0) {
    TTF_Quit();
  }
}

void addUniquePath(std::vector<std::string> &paths, std::string path) {
  if (path.empty()) {
    return;
  }
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
    paths.push_back(std::move(path));
  }
}

bool canReadFile(const std::string &path) {
  UniqueResource<SDL_RWops, SDL_RWclose> rw(SDL_RWFromFile(path.c_str(), "rb"));
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

std::vector<std::string> fontFallbackPaths(const std::string &primaryPath) {
  std::vector<std::string> paths;
  addUniquePath(paths, primaryPath);
  addUniquePath(paths, "assets/fonts/notosansjp.ttf");
  addUniquePath(paths, "assets/fonts/notosanskr.ttf");
  addUniquePath(paths, "assets/fonts/notosanssymbols2.ttf");
  addUniquePath(paths, "assets/fonts/arial.ttf");
  for (auto &path : systemFontFallbackPaths()) {
    addUniquePath(paths, std::move(path));
  }
  return paths;
}

std::string fontCacheKey(const std::string &path, int fontSize) {
  return path + "#" + std::to_string(fontSize);
}

TTF_Font *acquireFontCandidate(const std::string &path, int fontSize,
                               bool required) {
  if (!required && !canReadFile(path)) {
    return nullptr;
  }

  const std::string key = fontCacheKey(path, fontSize);
  {
    std::lock_guard<std::mutex> lock(g_fontCacheMutex);
    auto cached = g_fontCache.find(key);
    if (cached != g_fontCache.end()) {
      ++cached->second.refCount;
      return cached->second.font;
    }
  }

  TTF_Font *opened = TTF_OpenFont(path.c_str(), fontSize);
  if (opened == nullptr && (required || canReadFile(path))) {
    SDL_Log("Failed to load font '%s': %s", path.c_str(), TTF_GetError());
  }
  if (opened != nullptr) {
    std::lock_guard<std::mutex> lock(g_fontCacheMutex);
    auto [cached, inserted] = g_fontCache.emplace(key, CachedFont{opened, 1});
    if (!inserted) {
      ++cached->second.refCount;
      TTF_CloseFont(opened);
      return cached->second.font;
    }
  }
  return opened;
}

void releaseFontCandidate(const std::string &path, int fontSize,
                          TTF_Font *font) {
  if (font == nullptr) {
    return;
  }

  const std::string key = fontCacheKey(path, fontSize);
  std::lock_guard<std::mutex> lock(g_fontCacheMutex);
  auto cached = g_fontCache.find(key);
  if (cached == g_fontCache.end()) {
    TTF_CloseFont(font);
    return;
  }

  --cached->second.refCount;
  if (cached->second.refCount <= 0) {
    TTF_CloseFont(cached->second.font);
    g_fontCache.erase(cached);
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

int sizeUtf8Width(TTF_Font *font, const std::string &utf8) {
  if (font == nullptr || utf8.empty()) {
    return 0;
  }

  int width = 0;
  if (TTF_SizeUTF8(font, utf8.c_str(), &width, nullptr) != 0) {
    return 0;
  }
  return width;
}

} // namespace

TextView::TextView(const std::string &fontPath, int fontSize)
    : View(), texture(BGFX_INVALID_HANDLE) {
  this->fontSize = fontSize;
  fallbackFontPaths = fontFallbackPaths(fontPath);
  ttfInitialized = acquireTtf();
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
}

TextView::~TextView() {
  if (bgfx::isValid(texture)) {
    bgfx::destroy(texture);
  }

  for (auto &face : fontFaces) {
    if (face.font != nullptr) {
      releaseFontCandidate(face.path, fontSize, face.font);
      face.font = nullptr;
    }
  }
  font = nullptr;
  if (ttfInitialized) {
    releaseTtf();
  }
}

void TextView::setText(const std::string &newText) {
  if (newText == text) {
    return;
  }
  this->text = newText;
  marqueeStartedAt = SDL_GetTicks64();
  createTexture();
}

void TextView::renderImpl(RenderContext &context) {
  if (!bgfx::isValid(texture)) {
    return;
  }

  SDL_Rect drawRect = resolvedTextRect();
  const bool clip =
      overflow != TextOverflow::Visible && getWidth() > 0 && getHeight() > 0;
  if (overflow == TextOverflow::Marquee && !wrapEnabled &&
      rect.w > getWidth()) {
    drawRect.x =
        getX() - static_cast<int>(std::round(marqueeOffset(getWidth())));
  }

  const auto submitText = [this, &context, &drawRect]() {
    rendering::PosTexVertex vertices[] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},                           // Top-left
        {static_cast<float>(drawRect.w), 0.0f, 0.0f, 1.0f, 0.0f}, // Top-right
        {static_cast<float>(drawRect.w), static_cast<float>(drawRect.h), 0.0f,
         1.0f, 1.0f},                                            // Bottom-right
        {0.0f, static_cast<float>(drawRect.h), 0.0f, 0.0f, 1.0f} // Bottom-left
    };

    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer tib;
    bgfx::allocTransientVertexBuffer(&tvb, 4, rendering::PosTexVertex::ms_decl);
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tvb.data, vertices, sizeof(vertices));
    bx::memCopy(tib.data, indices, sizeof(indices));

    float translate[16];
    bx::mtxTranslate(translate, drawRect.x, drawRect.y, 0.0f);
    bgfx::setTransform(translate);
    bgfx::setTexture(0, s_texColor, texture);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);

    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    static const bgfx::ProgramHandle kProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
    bgfx::submit(rendering::ui_view, kProgram);
  };

  if (clip) {
    ScissorScope scissor(context, getX(), getY(), getWidth(), getHeight());
    submitText();
  } else {
    submitText();
  }
}

SDL_Rect TextView::resolvedTextRect() const {
  const int contentHeight = rect.h > 0 ? rect.h : textLineHeight();
  SDL_Rect drawRect = {getX(), getY(), rect.w, contentHeight};
  const int width = getWidth();
  const int height = getHeight();

  switch (align) {
  case TextAlign::LEFT:
    break;
  case TextAlign::CENTER:
    drawRect.x += (width - rect.w) / 2;
    break;
  case TextAlign::RIGHT:
    drawRect.x += width - rect.w;
    break;
  }

  switch (valign) {
  case TextVAlign::TOP:
    break;
  case TextVAlign::MIDDLE:
    drawRect.y += (height - contentHeight) / 2;
    break;
  case TextVAlign::BOTTOM:
    drawRect.y += height - contentHeight;
    break;
  }

  return drawRect;
}

int TextView::textLineHeight() const {
  return std::max(fontLineHeight, fontAscent + fontDescent);
}

void TextView::includeFontMetrics(TTF_Font *loadedFont) {
  if (loadedFont == nullptr) {
    return;
  }

  fontLineHeight = std::max(fontLineHeight, TTF_FontHeight(loadedFont));
  fontAscent = std::max(fontAscent, TTF_FontAscent(loadedFont));
  fontDescent = std::max(fontDescent, -TTF_FontDescent(loadedFont));
}

void TextView::includeIOSSystemFontMetrics() {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (iosSystemFontMetricsIncluded) {
    return;
  }

  const IOSSystemTextMetrics metrics = GetIOSSystemTextMetrics(fontSize);
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
  TTF_Font *opened = acquireFontCandidate(path, fontSize, required);
  if (opened == nullptr) {
    return nullptr;
  }

  if (font == nullptr) {
    font = opened;
  }
  fontFaces.push_back({opened, path});
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
                                         const std::string &utf8) {
  if (utf8.empty()) {
    return 0;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (source.iosSystemFont) {
    includeIOSSystemFontMetrics();
    return MeasureIOSSystemTextWidth(utf8, fontSize);
  }
#endif

  return sizeUtf8Width(source.font, utf8);
}

int TextView::fontSourceAscent(const SelectedFont &source) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (source.iosSystemFont) {
    includeIOSSystemFontMetrics();
    return iosSystemFontAscent;
  }
#endif

  return source.font == nullptr ? 0 : TTF_FontAscent(source.font);
}

SDL_Surface *TextView::renderFontSourceTextSurface(const SelectedFont &source,
                                                   const std::string &utf8) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  if (source.iosSystemFont) {
    includeIOSSystemFontMetrics();
    return RenderIOSSystemTextSurface(utf8, fontSize, color);
  }
#endif

  if (source.font == nullptr || utf8.empty()) {
    return nullptr;
  }
  return TTF_RenderUTF8_Blended(source.font, utf8.c_str(), color);
}

TextView::SelectedFont TextView::selectFont(Uint32 codepoint) {
  if (fontFaces.empty()) {
    return {};
  }

  auto cached = fontSelectionCache.find(codepoint);
  if (cached != fontSelectionCache.end()) {
    return cached->second;
  }

  for (const auto &face : fontFaces) {
    if (face.font != nullptr && TTF_GlyphIsProvided32(face.font, codepoint)) {
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
    if (opened != nullptr && TTF_GlyphIsProvided32(opened, codepoint)) {
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
  if (font == nullptr) {
    return false;
  }

  size_t index = 0;
  Utf8Token token;
  while (decodeNextUtf8(utf8, index, token)) {
    if (isExplicitLineBreak(token.codepoint)) {
      continue;
    }
    if (!TTF_GlyphIsProvided32(font, token.codepoint)) {
      return false;
    }
  }
  return true;
}

int TextView::measureTextWidth(const std::string &utf8) {
  if (utf8.empty() || fontFaces.empty()) {
    return 0;
  }

  int totalWidth = 0;
  SelectedFont runSource;
  std::string runText;
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
      totalWidth += measureFontSourceTextWidth(runSource, runText);
      runText.clear();
    }
    runSource = tokenSource;
    runText += token.bytes;
  }

  if (!runText.empty()) {
    totalWidth += measureFontSourceTextWidth(runSource, runText);
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

    const int currentWidth = measureTextWidth(currentLine);
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

  TextLineMetrics metrics = {fontAscent, fontDescent, textLineHeight()};
  if (metrics.height <= 0) {
    return nullptr;
  }

  const std::vector<std::string> lines =
      wrapWidth > 0 ? wrappedTextLines(wrapWidth) : wrappedTextLines(0);
  int width = 0;
  for (const auto &line : lines) {
    width = std::max(width, measureTextWidth(line));
  }

  const int targetWidth = std::max(1, width);
  const int targetHeight =
      std::max(1, metrics.height * static_cast<int>(lines.size()));
  SurfacePtr surface(SDL_CreateRGBSurfaceWithFormat(
      0, targetWidth, targetHeight, 32, SDL_PIXELFORMAT_BGRA32));
  if (surface == nullptr) {
    SDL_Log("Failed to create text fallback surface: %s", SDL_GetError());
    return nullptr;
  }

  SDL_FillRect(surface.get(), nullptr,
               SDL_MapRGBA(surface->format, 0, 0, 0, 0));

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

    int x = 0;
    const int lineTop = metrics.height * static_cast<int>(lineIndex);
    for (const auto &run : runs) {
      if (!hasFontSource(run.source) || run.text.empty()) {
        continue;
      }

      SurfacePtr runSurface(renderFontSourceTextSurface(run.source, run.text));
      if (runSurface == nullptr) {
        SDL_Log("Failed to render fallback text run: %s", TTF_GetError());
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
    marqueeStartedAt = SDL_GetTicks64();
  }

  const float scrollDurationMs =
      static_cast<float>(overflowWidth) / kMarqueePixelsPerSecond * 1000.0f;
  const Uint64 scrollMs =
      std::max<Uint64>(1, static_cast<Uint64>(std::round(scrollDurationMs)));
  const Uint64 cycleMs = kMarqueeStartDelayMs + scrollMs + kMarqueeEdgeDelayMs +
                         scrollMs + kMarqueeEdgeDelayMs;
  Uint64 phase = (SDL_GetTicks64() - marqueeStartedAt) % cycleMs;

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
  if (newColor.r == color.r && newColor.g == color.g && newColor.b == color.b &&
      newColor.a == color.a) {
    return;
  }
  this->color = newColor;
  createTexture(); // Update the texture since newColor has changed
}

void TextView::createTexture(bool markDirty, bool force,
                             int requestedWrapWidth) {
  const int previousWidth = rect.w;
  const int previousHeight = rect.h;
  const int effectiveWrapWidth =
      wrapEnabled ? std::max(0, requestedWrapWidth >= 0 ? requestedWrapWidth
                                                        : currentWrapWidth)
                  : 0;
  if (!force && effectiveWrapWidth == currentWrapWidth &&
      bgfx::isValid(texture)) {
    return;
  }

  currentWrapWidth = effectiveWrapWidth;
  if (bgfx::isValid(texture)) {
    bgfx::destroy(texture);
    texture = BGFX_INVALID_HANDLE;
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
  SurfacePtr surface(nullptr);
  int fallbackSurfaceWidth = 0;
  int fallbackSurfaceHeight = 0;
  const bool usePrimaryFont = font != nullptr && primaryFontSupportsText(text);
  if (usePrimaryFont && wrapEnabled && effectiveWrapWidth > 0) {
    surface.reset(TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), color,
                                                 effectiveWrapWidth));
  } else if (usePrimaryFont) {
    surface.reset(TTF_RenderUTF8_Blended(font, text.c_str(), color));
  } else {
    surface.reset(renderFallbackTextSurface(
        wrapEnabled && effectiveWrapWidth > 0 ? effectiveWrapWidth : 0,
        fallbackSurfaceWidth, fallbackSurfaceHeight));
  }
  if (surface == nullptr && usePrimaryFont && fontFaces.size() > 1) {
    surface.reset(renderFallbackTextSurface(
        wrapEnabled && effectiveWrapWidth > 0 ? effectiveWrapWidth : 0,
        fallbackSurfaceWidth, fallbackSurfaceHeight));
  }
  if (!surface) {
    SDL_Log("Failed to render text: %s", TTF_GetError());
    return;
  }
  rect.w = fallbackSurfaceHeight > 0 ? fallbackSurfaceWidth : surface->w;
  rect.h = fallbackSurfaceHeight > 0 ? fallbackSurfaceHeight : surface->h;
  texture = rendering::sdlSurfaceToBgfxTexture(surface.get());
  if (markDirty && (rect.w != previousWidth || rect.h != previousHeight)) {
    YGNodeMarkDirty(getNode());
    applyYogaLayoutFromRoot();
  }
}

YGSize TextView::measureFunc(YGNodeConstRef node, float width,
                             YGMeasureMode widthMode, float height,
                             YGMeasureMode heightMode) {
  auto *view = static_cast<TextView *>(YGNodeGetContext(node));
  (void)height;
  (void)heightMode;
  if (view->wrapEnabled && widthMode != YGMeasureModeUndefined &&
      width > 0.0f) {
    view->createTexture(false, false, static_cast<int>(std::floor(width)));
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

void TextView::setAlign(TextAlign newAlign) { this->align = newAlign; }

void TextView::setVAlign(TextVAlign newVAlign) { this->valign = newVAlign; }

void TextView::setOverflow(TextOverflow newOverflow) {
  if (overflow == newOverflow) {
    return;
  }
  overflow = newOverflow;
  marqueeStartedAt = SDL_GetTicks64();
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
  createTexture();
}
