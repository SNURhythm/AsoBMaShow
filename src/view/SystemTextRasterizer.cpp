#include "SystemTextRasterizer.h"
#include "SdlTtfRuntime.h"
#include "../RAII.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <iterator>
#include <string>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#elif defined(__ANDROID__)
#include <jni.h>
#include <atomic>
#include <cstdint>
#else
#include <SDL3_ttf/SDL_ttf.h>
#endif

namespace text_runtime {
namespace {
template <auto Cleanup, typename T>
UniqueResource<T, Cleanup> owned(T *value) { return UniqueResource<T, Cleanup>(value); }

bool fits(int width, int height, std::size_t maximumBytes) {
  return width > 0 && height > 0 &&
         static_cast<std::size_t>(width) <= maximumBytes / 4U /
                                               static_cast<std::size_t>(height);
}

std::string utf8For(char32_t c) {
  if (c <= 0x7f) return {static_cast<char>(c)};
  if (c <= 0x7ff) return {static_cast<char>(0xc0 | (c >> 6)), static_cast<char>(0x80 | (c & 63))};
  if (c <= 0xffff) return {static_cast<char>(0xe0 | (c >> 12)), static_cast<char>(0x80 | ((c >> 6) & 63)), static_cast<char>(0x80 | (c & 63))};
  return {static_cast<char>(0xf0 | (c >> 18)), static_cast<char>(0x80 | ((c >> 12) & 63)), static_cast<char>(0x80 | ((c >> 6) & 63)), static_cast<char>(0x80 | (c & 63))};
}
}

#if defined(__APPLE__)
struct SystemTextRasterizer::Impl {
  CTFontRef font;
  explicit Impl(int pointSize)
      : font(CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, pointSize, nullptr)) {}
  ~Impl() { if (font) CFRelease(font); }

  std::optional<SystemTextRaster> render(
      std::string_view utf8, std::array<unsigned char, 4> color,
      std::size_t maximumBytes, bool colorOnly) {
    if (!font || utf8.empty() || utf8.size() > static_cast<std::size_t>(LONG_MAX)) return std::nullopt;
    auto string = owned<CFRelease>(CFStringCreateWithBytes(
        nullptr, reinterpret_cast<const UInt8 *>(utf8.data()),
        static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false));
    if (!string) return std::nullopt;
    const void *keys[]{kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
    const void *values[]{font, kCFBooleanTrue};
    auto attributes = owned<CFRelease>(CFDictionaryCreate(
        nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    auto attributed = owned<CFRelease>(CFAttributedStringCreate(
        nullptr, string.get(), attributes.get()));
    auto line = owned<CFRelease>(CTLineCreateWithAttributedString(attributed.get()));
    if (!line) return std::nullopt;
    if (colorOnly) {
      bool colorFont = false;
      const auto runs = CTLineGetGlyphRuns(line.get());
      for (CFIndex i = 0; i < CFArrayGetCount(runs); ++i) {
        const auto run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, i));
        const auto runFont = static_cast<CTFontRef>(CFDictionaryGetValue(
            CTRunGetAttributes(run), kCTFontAttributeName));
        if (runFont && (CTFontGetSymbolicTraits(runFont) & kCTFontColorGlyphsTrait)) {
          colorFont = true;
        }
        for (CFIndex glyph = 0; glyph < CTRunGetGlyphCount(run); ++glyph) {
          CGGlyph index = 0;
          CTRunGetGlyphs(run, CFRangeMake(glyph, 1), &index);
          if (index == 0) return std::nullopt;
        }
      }
      if (!colorFont) return std::nullopt;
    }
    CGFloat lineAscent = 0, lineDescent = 0, leading = 0;
    const double width = std::ceil(CTLineGetTypographicBounds(
        line.get(), &lineAscent, &lineDescent, &leading));
    const double ascent = std::ceil(colorOnly ? std::max(lineAscent, CTFontGetAscent(font))
                                             : CTFontGetAscent(font));
    const double descent = colorOnly ? std::max(lineDescent, CTFontGetDescent(font))
                                     : CTFontGetDescent(font);
    const double height = colorOnly
        ? std::ceil(ascent + descent + std::max(leading, CTFontGetLeading(font)))
        : std::max(std::ceil(CTFontGetAscent(font) + CTFontGetDescent(font) +
                             CTFontGetLeading(font)),
                   ascent + std::ceil(CTFontGetDescent(font)));
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height <= 0 ||
        width >= std::numeric_limits<int>::max() || height >= std::numeric_limits<int>::max()) return std::nullopt;
    SystemTextRaster result{.width = std::max(1, static_cast<int>(width)),
                            .height = static_cast<int>(height),
                            .ascent = static_cast<int>(ascent),
                            .advance = static_cast<int>(width)};
    if (!fits(result.width, result.height, maximumBytes)) return std::nullopt;
    result.rgba.resize(static_cast<std::size_t>(result.width) * result.height * 4U);
    auto space = owned<CGColorSpaceRelease>(CGColorSpaceCreateDeviceRGB());
    auto context = owned<CGContextRelease>(CGBitmapContextCreate(
        result.rgba.data(), result.width, result.height, 8, result.width * 4U,
        space.get(), static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big) | static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast)));
    if (!context) return std::nullopt;
    CGContextSetShouldAntialias(context.get(), true);
    CGContextSetAllowsAntialiasing(context.get(), true);
    CGContextSetRGBFillColor(context.get(), color[0] / 255.0, color[1] / 255.0,
                            color[2] / 255.0, 1.0);
    CGContextSetAlpha(context.get(), color[3] / 255.0);
    CGContextSetTextMatrix(context.get(), CGAffineTransformIdentity);
    CGContextSetTextPosition(context.get(), 0, result.height - result.ascent);
    CTLineDraw(line.get(), context.get());
    // Quartz writes premultiplied pixels; SDL/bgfx text uses straight alpha.
    for (std::size_t i = 0; i < result.rgba.size(); i += 4U) {
      const unsigned alpha = result.rgba[i + 3U];
      if (alpha == 0) continue;
      for (std::size_t channel = 0; channel < 3; ++channel) {
        result.rgba[i + channel] = static_cast<unsigned char>(std::min(
            255U, (result.rgba[i + channel] * 255U + alpha / 2U) / alpha));
      }
    }
    return result;
  }
};
#elif defined(__ANDROID__)
namespace {
struct AndroidTextBridge {
  JavaVM *vm = nullptr;
  jclass type = nullptr;
  jmethodID rasterize = nullptr;
  std::once_flag once;
  std::atomic<bool> ready{false};
};
AndroidTextBridge androidTextBridge;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_AndroidTextRasterizer_initializeNative(
    JNIEnv *env, jclass type) {
  try {
    std::call_once(androidTextBridge.once, [&] {
      auto method = env->GetStaticMethodID(type, "rasterize", "([BIIJZZ)[I");
      if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
      JavaVM *vm = nullptr;
      if (!method || env->GetJavaVM(&vm) != JNI_OK) return;
      auto global = static_cast<jclass>(env->NewGlobalRef(type));
      if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
      if (!global) return;
      // Process lifetime, with no Activity reference. Initialize on the Java
      // loading thread so worker threads never depend on FindClass loaders.
      androidTextBridge.vm = vm;
      androidTextBridge.type = global;
      androidTextBridge.rasterize = method;
      androidTextBridge.ready.store(true, std::memory_order_release);
    });
  } catch (...) { return JNI_FALSE; }
  return androidTextBridge.ready.load(std::memory_order_acquire);
}

struct SystemTextRasterizer::Impl {
  int pointSize;
  explicit Impl(int size) : pointSize(size) {}

  std::optional<SystemTextRaster> render(
      std::string_view utf8, std::array<unsigned char, 4> color,
      std::size_t maximumBytes, bool colorOnly, bool metricsOnly = false) {
    if (utf8.empty() || utf8.size() > 65536 || maximumBytes < 4 ||
        !androidTextBridge.ready.load(std::memory_order_acquire)) return std::nullopt;
    maximumBytes = std::min(maximumBytes, std::size_t{32U * 1024U * 1024U});
    JNIEnv *env = nullptr;
    const auto status = androidTextBridge.vm->GetEnv(
        reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    bool attached = false;
    if (status == JNI_EDETACHED) {
      if (androidTextBridge.vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return std::nullopt;
      attached = true;
    } else if (status != JNI_OK) return std::nullopt;
    ScopeExit detach([&] { if (attached) androidTextBridge.vm->DetachCurrentThread(); });
    if (env->PushLocalFrame(8) < 0) { env->ExceptionClear(); return std::nullopt; }
    ScopeExit frame([&] { env->PopLocalFrame(nullptr); });
    const auto failed = [&] {
      if (!env->ExceptionCheck()) return false;
      env->ExceptionClear();
      return true;
    };
    auto input = env->NewByteArray(static_cast<jsize>(utf8.size()));
    if (failed() || !input) return std::nullopt;
    env->SetByteArrayRegion(input, 0, static_cast<jsize>(utf8.size()),
                           reinterpret_cast<const jbyte *>(utf8.data()));
    if (failed()) return std::nullopt;
    const auto argb = (std::uint32_t(color[3]) << 24) |
                      (std::uint32_t(color[0]) << 16) |
                      (std::uint32_t(color[1]) << 8) | color[2];
    auto output = static_cast<jintArray>(env->CallStaticObjectMethod(
        androidTextBridge.type, androidTextBridge.rasterize, input,
        static_cast<jint>(pointSize), static_cast<jint>(argb),
        static_cast<jlong>(maximumBytes), static_cast<jboolean>(colorOnly),
        static_cast<jboolean>(metricsOnly)));
    if (failed() || !output) return std::nullopt;
    const auto length = env->GetArrayLength(output);
    if (length < 5) return std::nullopt;
    jint header[5];
    env->GetIntArrayRegion(output, 0, 5, header);
    if (failed() || !fits(header[0], header[1], maximumBytes) ||
        header[0] > 8192 || header[1] > 8192 || header[2] < 0 ||
        header[2] > header[1] || header[3] < 0 || header[3] > header[0] ||
        header[4] > 0 || header[4] < -header[0]) return std::nullopt;
    const auto count = static_cast<std::size_t>(header[0]) * header[1];
    if (static_cast<std::size_t>(length) != 5U + (metricsOnly ? 0U : count)) return std::nullopt;
    SystemTextRaster result{.width = header[0], .height = header[1],
                            .ascent = header[2], .advance = header[3], .offsetX = header[4]};
    if (!metricsOnly) {
      result.rgba.resize(count * 4U);
      std::array<jint, 1024> pixels;
      for (std::size_t offset = 0; offset < count; offset += pixels.size()) {
        const auto batch = std::min(pixels.size(), count - offset);
        env->GetIntArrayRegion(output, static_cast<jsize>(5U + offset),
                               static_cast<jsize>(batch), pixels.data());
        if (failed()) return std::nullopt;
        for (std::size_t i = 0; i < batch; ++i) {
          const auto pixel = static_cast<std::uint32_t>(pixels[i]);
          const auto destination = (offset + i) * 4U;
          result.rgba[destination] = (pixel >> 16) & 255;
          result.rgba[destination + 1] = (pixel >> 8) & 255;
          result.rgba[destination + 2] = pixel & 255;
          result.rgba[destination + 3] = pixel >> 24;
        }
      }
    }
    return result;
  }
};
#else
struct SystemTextRasterizer::Impl {
  std::vector<TTF_Font *> fonts;
  explicit Impl(int pointSize) {
    OperationGuard operation;
#if defined(_WIN32)
    const char *paths[]{"C:/Windows/Fonts/seguiemj.ttf"};
#else
    const char *paths[]{"/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
                        "/usr/share/fonts/noto/NotoColorEmoji.ttf"};
#endif
    fonts.reserve(std::size(paths));
    for (const auto *path : paths) {
      if (auto *font = TTF_OpenFont(path, pointSize)) fonts.push_back(font);
    }
  }
  ~Impl() {
    { OperationGuard operation; for (auto *font : fonts) TTF_CloseFont(font); }
  }
  std::optional<SystemTextRaster> render(char32_t codepoint, std::size_t maximumBytes) {
    OperationGuard operation;
    for (auto *font : fonts) {
      if (!TTF_FontHasGlyph(font, codepoint)) continue;
      const auto utf8 = utf8For(codepoint);
      int width = 0, height = 0;
      if (!TTF_GetStringSize(font, utf8.data(), utf8.size(), &width, &height) ||
          !fits(width, height, maximumBytes)) continue;
      TTF_ImageType type = TTF_IMAGE_INVALID;
      auto glyphImage = owned<SDL_DestroySurface>(TTF_GetGlyphImage(font, codepoint, &type));
      if (!glyphImage || type != TTF_IMAGE_COLOR) continue;
      auto raw = owned<SDL_DestroySurface>(TTF_RenderText_Blended(
          font, utf8.data(), utf8.size(), SDL_Color{255, 255, 255, 255}));
      if (!raw || !fits(raw->w, raw->h, maximumBytes)) continue;
      auto surface = owned<SDL_DestroySurface>(SDL_ConvertSurface(raw.get(), SDL_PIXELFORMAT_RGBA32));
      if (!surface || !SDL_LockSurface(surface.get())) continue;
      ScopeExit unlock([&] { SDL_UnlockSurface(surface.get()); });
      SystemTextRaster result{.width = surface->w, .height = surface->h,
                              .ascent = TTF_GetFontAscent(font), .advance = width};
      result.rgba.resize(static_cast<std::size_t>(result.width) * result.height * 4U);
      for (int y = 0; y < result.height; ++y) {
        std::copy_n(static_cast<unsigned char *>(surface->pixels) + y * surface->pitch,
                    result.width * 4U, result.rgba.data() + static_cast<std::size_t>(y) * result.width * 4U);
      }
      return result;
    }
    return std::nullopt;
  }
};
#endif

SystemTextRasterizer::SystemTextRasterizer(int pointSize) : pointSize_(pointSize) {
#if !defined(__APPLE__) && !defined(__ANDROID__)
  if (pointSize > 0 && pointSize <= 8192) ownsTtfRuntime_ = acquire();
#endif
}
SystemTextRasterizer::~SystemTextRasterizer() {
  impl_.reset();
  if (ownsTtfRuntime_) release();
}

std::optional<SystemTextRaster> SystemTextRasterizer::colorGlyph(
    char32_t codepoint, std::size_t maximumBytes) {
  if (pointSize_ <= 0 || pointSize_ > 8192 || codepoint <= 0x7f || codepoint > 0x10ffff ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff)) return std::nullopt;
#if !defined(__APPLE__) && !defined(__ANDROID__)
  if (!ownsTtfRuntime_) return std::nullopt;
#endif
  if (!impl_) impl_ = std::make_unique<Impl>(pointSize_);
#if defined(__APPLE__) || defined(__ANDROID__)
  return impl_->render(utf8For(codepoint), {255, 255, 255, 255}, maximumBytes, true);
#else
  return impl_->render(codepoint, maximumBytes);
#endif
}

#if defined(__APPLE__) || defined(__ANDROID__)
std::optional<SystemTextRaster> SystemTextRasterizer::text(
    std::string_view utf8, std::array<unsigned char, 4> color,
    std::size_t maximumBytes) {
  if (pointSize_ <= 0 || pointSize_ > 8192) return std::nullopt;
  if (!impl_) impl_ = std::make_unique<Impl>(pointSize_);
  return impl_->render(utf8, color, maximumBytes, false);
}
#endif
#if defined(__ANDROID__)
std::optional<SystemTextRaster> SystemTextRasterizer::measure(
    std::string_view utf8, std::size_t maximumBytes) {
  if (pointSize_ <= 0 || pointSize_ > 8192) return std::nullopt;
  if (!impl_) impl_ = std::make_unique<Impl>(pointSize_);
  return impl_->render(utf8, {255, 255, 255, 255}, maximumBytes, false, true);
}
#endif
} // namespace text_runtime
