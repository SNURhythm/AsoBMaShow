#include "view/TextView.h"
#include "view/SystemTextRasterizer.h"
#include "skin/beatoraja/SkinTextAtlas.h"

#include <jni.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace {
constexpr std::size_t budget = 1024U * 1024U;
std::atomic<int> failures{0};
void expect(bool value, const char *message) {
  if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
bool colored(const std::vector<unsigned char> &pixels) {
  for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
    if (pixels[i + 3] > 64 &&
        (pixels[i] != pixels[i + 1] || pixels[i + 1] != pixels[i + 2])) return true;
  }
  return false;
}
bool hasInk(const text_runtime::SystemTextRaster &raster) {
  for (std::size_t i = 3; i < raster.rgba.size(); i += 4) if (raster.rgba[i]) return true;
  return false;
}
class FontProbe : public TextView {
public:
  explicit FontProbe(const std::string &directory)
      : TextView(directory + "/assets/fonts/roboto-condensed-regular.ttf", 32) {
    // app_process has no SDL Activity asset manager. Map the identical APK
    // assets to their extracted paths, omitting unreadable optional files.
    for (auto &path : fallbackFontPaths) if (path.front() != '/') path = directory + "/" + path;
    std::erase_if(fallbackFontPaths, [](const auto &path) { return access(path.c_str(), R_OK) != 0; });
    setDeferredTextureMaterialization(true);
  }
  void forceNative(const std::string &text) {
    const char *cursor = text.c_str();
    std::size_t remaining = text.size();
    while (remaining != 0) {
      const auto codepoint = SDL_StepUTF8(&cursor, &remaining);
      fontSelectionCache[codepoint] = {nullptr, true};
    }
    includeNativeSystemFontMetrics();
  }
  int baseline() const { return fontAscent; }
  bool nativeEmoji(char32_t codepoint) { return selectFont(codepoint).nativeSystemFont; }
  SDL_Surface *raster() { int width = 0, height = 0; return renderFallbackTextSurface(0, width, height); }
  int measuredWidth(const std::string &value) { return measureRasterTextWidth(value); }
};

void testNative(JavaVM *vm, int unavailableEmoji) {
  text_runtime::SystemTextRasterizer rasterizer(32);
  if (unavailableEmoji >= 0) expect(!rasterizer.colorGlyph(unavailableEmoji, budget),
                                    "recognized but unavailable emoji is rejected through JNI");
  const auto smile = rasterizer.colorGlyph(U'\U0001f642', budget);
  expect(smile && colored(smile->rgba), "native smile has color pixels");
  const auto grin = rasterizer.colorGlyph(U'\U0001f600', budget);
  expect(grin && colored(grin->rgba), "native grin has color pixels");
  const auto black = rasterizer.colorGlyph(U'\U0001f5a4', budget);
  expect(black && hasInk(*black), "monochrome emoji are retained");
  expect(!rasterizer.colorGlyph(U'A', budget) && !rasterizer.colorGlyph(U'\u03a9', budget) &&
             !rasterizer.colorGlyph(U'\U0010ffff', budget) && !rasterizer.colorGlyph(0xd800, budget),
         "ordinary and unavailable glyphs are excluded from emoji fallback");
  expect(!rasterizer.colorGlyph(U'\U0001f642', 1) &&
             !rasterizer.text(std::string(65537, 'A'), {255, 255, 255, 255}, budget) &&
             !text_runtime::SystemTextRasterizer(8193).colorGlyph(U'\U0001f642', budget),
         "rasterization observes byte, input and point-size bounds");
  if (smile) {
    expect(!rasterizer.colorGlyph(U'\U0001f642', smile->rgba.size() - 1),
           "actual pixel allocation cannot exceed caller budget");
  }
  const auto red = rasterizer.text("A", {255, 0, 0, 128}, budget);
  expect(red && hasInk(*red), "native text has visible ink");
  if (red) {
    bool tint = true;
    for (std::size_t i = 0; i < red->rgba.size(); i += 4) {
      if (red->rgba[i + 3] == 0) continue;
      tint &= red->rgba[i] >= 253 && red->rgba[i + 1] == 0 &&
              red->rgba[i + 2] == 0 && red->rgba[i + 3] <= 128;
    }
    expect(tint, "native tint uses straight alpha");
  }
  const auto translucentEmoji = rasterizer.text("🙂", {255, 0, 0, 128}, budget);
  expect(translucentEmoji && colored(translucentEmoji->rgba), "text tint preserves intrinsic emoji color");
  if (translucentEmoji) {
    bool boundedAlpha = true;
    for (std::size_t i = 3; i < translucentEmoji->rgba.size(); i += 4) {
      boundedAlpha &= translucentEmoji->rgba[i] <= 128;
    }
    expect(boundedAlpha, "emoji respects requested alpha");
  }
  for (const auto *text : {"Ag", "🙂😀", "🙂\xef\xb8\x8f", "👨‍👩‍👧"}) {
    const auto metrics = rasterizer.measure(text, budget);
    const auto image = rasterizer.text(text, {255, 255, 255, 255}, budget);
    expect(metrics && image && metrics->rgba.empty() &&
               metrics->width == image->width && metrics->height == image->height &&
               metrics->ascent == image->ascent && metrics->advance == image->advance &&
               metrics->offsetX == image->offsetX,
           "measurement matches rendering, including selectors and joined runs");
  }
  std::vector<std::thread> workers;
  for (int i = 0; i < 3; ++i) workers.emplace_back([vm] {
    JNIEnv *env = nullptr;
    expect(vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED,
           "native worker begins detached");
    text_runtime::SystemTextRasterizer worker(24);
    const auto glyph = worker.colorGlyph(U'\U0001f642', budget);
    expect(glyph && colored(glyph->rgba), "native worker renders through JNI");
    expect(vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED,
           "renderer detaches only its own JNI worker attachment");
  });
  for (auto &worker : workers) worker.join();
}

void testBuiltIn(const std::string &directory) {
  FontProbe view(directory);
  expect(view.nativeEmoji(U'\U0001f642') && view.nativeEmoji(U'\U0001f600'),
         "built-in UI selects native Android emoji fallback");
  const std::string text = "Ag 🙂 😀 ♥️ Zg";
  view.setText(text);
  auto *surface = view.raster();
  expect(surface && surface->w == view.measuredWidth(text), "built-in raster width matches measurement");
  if (!surface) return;
  auto *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
  if (rgba) {
    std::vector<unsigned char> pixels;
    for (int y = 0; y < rgba->h; ++y) {
      const auto *row = static_cast<unsigned char *>(rgba->pixels) + y * rgba->pitch;
      pixels.insert(pixels.end(), row, row + rgba->w * 4U);
    }
    expect(colored(pixels), "built-in text preserves emoji color");
    SDL_DestroySurface(rgba);
  } else expect(false, "built-in surface converts to RGBA");
  auto *background = SDL_CreateSurface(surface->w, surface->h, SDL_PIXELFORMAT_RGBA32);
  if (background) {
    SDL_FillSurfaceRect(background, nullptr, SDL_MapSurfaceRGBA(background, 35, 35, 35, 255));
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(surface, nullptr, background, nullptr);
    SDL_SaveBMP(background, (directory + "/builtin-emoji.bmp").c_str());
    SDL_DestroySurface(background);
  }
  SDL_DestroySurface(surface);
}

void testBuiltInInkBounds(const std::string &directory) {
  for (const auto *text : {"A\u030d\u030d\u030d\u030d", "བོད་སྐད་", "لأّ", "\u0f73"}) {
    FontProbe view(directory);
    view.forceNative(text);
    view.setText(text);
    auto *surface = view.raster();
    auto *rgba = surface ? SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32) : nullptr;
    // TextView rasterizes at twice its logical point size.
    text_runtime::SystemTextRasterizer renderer(64);
    const auto expected = renderer.text(text, {255, 255, 255, 255}, budget);
    expect(rgba && expected, "overhanging native run renders in TextView");
    if (rgba && expected) {
      const int yOffset = view.baseline() - expected->ascent;
      bool matches = rgba->w == expected->width && yOffset >= 0 &&
                     rgba->h >= expected->height + yOffset;
      if (matches) for (int y = 0; y < expected->height; ++y) {
        const auto *row = static_cast<unsigned char *>(rgba->pixels) + (y + yOffset) * rgba->pitch;
        matches &= std::equal(row, row + expected->width * 4U,
                              expected->rgba.data() + y * expected->width * 4U);
      }
      expect(matches, "built-in composition preserves native overhang and baseline");
      expect(view.measuredWidth(text) == rgba->w, "overhanging built-in width matches layout");
    }
    if (rgba) SDL_DestroySurface(rgba);
    if (surface) SDL_DestroySurface(surface);
  }
}

void testAtlases(const std::string &directory) {
  std::ifstream input(directory + "/assets/fonts/roboto-condensed-regular.ttf", std::ios::binary);
  const std::string encoded((std::istreambuf_iterator<char>(input)), {});
  const auto bytes = std::as_bytes(std::span(encoded));
  const auto scalable = skin::buildSkinTextAtlas(
      1, {.font = 1, .pointSize = 32, .fallbackChainDigest = "android-emoji-test"},
      {{.encoded = {bytes.begin(), bytes.end()}}}, {U'A', U'\U0001f642', U'\ufe0f'}, {});
  expect(scalable.atlas && colored(*scalable.atlas->pixels.rgba), "scalable skin has native color emoji");
  if (scalable.atlas) {
    const auto baseline = [](const auto &g) { return g.layoutOffsetY + g.region.h - g.bearingY; };
    expect(baseline(scalable.atlas->glyphs.at(U'A')) == baseline(scalable.atlas->glyphs.at(U'\U0001f642')),
           "scalable authored glyph and native emoji share a baseline");
    expect(scalable.atlas->glyphs.at(U'\ufe0f').advance == 0, "skin selector has no advance");
  }
  constexpr std::string_view descriptor =
      "info size=32\ncommon lineHeight=32 base=26 scaleW=8 scaleH=8 pages=1\n"
      "page id=0 file=page.png\n"
      "char id=65 x=0 y=0 width=8 height=8 xoffset=0 yoffset=2 xadvance=8 page=0\n"
      "char id=9633 x=0 y=0 width=8 height=8 xoffset=0 yoffset=2 xadvance=8 page=0\n";
  const auto parsed = skin::parseSkinBitmapFont(
      {.id = 2, .virtualPath = "emoji.fnt", .type = 2}, std::as_bytes(std::span(descriptor)),
      skin::SkinBitmapFontSourceFormat::BmFont);
  expect(parsed.font.has_value(), "bitmap fixture parses");
  if (!parsed.font) return;
  image_decode::DecodedImageData pixels{.width = 8, .height = 8,
      .rgba = std::make_shared<std::vector<unsigned char>>(8 * 8 * 4, 255)};
  const auto bitmap = skin::buildSkinBitmapTextAtlas(
      2, {.font = 2, .pointSize = 1, .fallbackChainDigest = "android-bitmap-emoji-test"},
      {{.font = *parsed.font, .pages = {{.physicalKey = "fixture", .pixels = pixels}}}},
      {U'A', U'\U0001f642', U'\ufe0f'}, {});
  expect(bitmap.atlas.has_value(), "bitmap atlas prepares");
  if (!bitmap.atlas) return;
  const auto &glyph = bitmap.atlas->glyphs.at(U'\U0001f642');
  expect(glyph.bitmapFontType == 0 && colored(*bitmap.atlas->pages[glyph.page].pixels->rgba) &&
             bitmap.atlas->glyphs.at(U'A').page == 0 && bitmap.atlas->glyphs.at(U'\ufe0f').advance == 0,
         "bitmap skin preserves authored glyphs and adds a native color page");
  text_runtime::SystemTextRasterizer rasterizer(32);
  const auto native = rasterizer.colorGlyph(U'\U0001f642', budget);
  expect(native && glyph.layoutOffsetY + glyph.region.h - native->ascent == -26,
         "bitmap native emoji uses the authored baseline");
}
} // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_snurhythm_asobmashow_EmojiDeviceTest_verify(JNIEnv *env, jclass, jstring path, jint unavailableEmoji) {
  const char *characters = env->GetStringUTFChars(path, nullptr);
  if (!characters) return 1;
  const std::string directory(characters);
  env->ReleaseStringUTFChars(path, characters);
  JavaVM *vm = nullptr;
  env->GetJavaVM(&vm);
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 640;
  init.resolution.height = 200;
  if (!bgfx::init(init)) return 1;
  try {
    testNative(vm, unavailableEmoji);
    testBuiltIn(directory);
    testBuiltInInkBounds(directory);
    testAtlases(directory);
  } catch (const std::exception &error) {
    ++failures;
    std::fprintf(stderr, "FAIL: exception: %s\n", error.what());
  }
  bgfx::shutdown();
  return failures.load();
}
