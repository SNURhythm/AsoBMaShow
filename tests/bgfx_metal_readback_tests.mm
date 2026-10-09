#import <Metal/Metal.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <chrono>
#include <cstdio>
#include <vector>

static bool checkExternalTextureReuse() {
  auto handle = bgfx::createTexture2D(32, 32, false, 1, bgfx::TextureFormat::RGBA8);
  bgfx::frame();
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                     width:32 height:32 mipmapped:NO];
  id<MTLTexture> external = [device newTextureWithDescriptor:descriptor];
  bgfx::overrideInternal(handle, (uintptr_t)external);
  id<MTLTexture> replacement = (id<MTLTexture>)bgfx::overrideInternal(
      handle, 64, 48, 1, bgfx::TextureFormat::RGBA8, 0);
  const bool passed = replacement != external && replacement.width == 64 && replacement.height == 48;
  if (!passed) fprintf(stderr, "FAIL: external texture slot was not recreated\n");
  bgfx::destroy(handle);
  bgfx::frame();
  bgfx::frame();
  [external release];
  [device release];
  return passed;
}

static bool checkReadback(uint16_t width, uint16_t height,
                          bgfx::TextureFormat::Enum format, bool mipmapped) {
  const uint64_t flags = BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST;
  auto source = bgfx::createTexture2D(width, height, mipmapped, 1, format);
  auto target = bgfx::createTexture2D(width, height, mipmapped, 1, format, flags);
  bgfx::frame();
  // Single-threaded rendering lets us inspect the recreated native resource.
  id<MTLTexture> native = (id<MTLTexture>)bgfx::overrideInternal(
      target, width, height, mipmapped ? 2 : 1, format, flags);
  bool passed = native != nil;
  if (@available(macOS 10.15, *)) {
    if (native.device.hasUnifiedMemory && !mipmapped && native.buffer == nil) {
      fprintf(stderr, "FAIL: readback texture has no shared buffer\n");
      passed = false;
    }
  }
  if (mipmapped && native.buffer != nil) {
    fprintf(stderr, "FAIL: mipmapped texture must use the fallback\n");
    passed = false;
  }
  double elapsed = 0;
  for (int frame = 0; frame < 24; ++frame) {
    const uint8_t mip = mipmapped ? frame % 2 : 0;
    const uint16_t w = width >> mip, h = height >> mip;
    std::vector<uint8_t> expected(size_t(w) * h * 4);
    for (size_t i = 0; i < expected.size(); ++i)
      expected[i] = uint8_t((i * 37 + i / (w * 4) + frame * 19) % 251);
    bgfx::updateTexture2D(source, 0, mip, 0, 0, w, h,
                         bgfx::copy(expected.data(), uint32_t(expected.size())));
    bgfx::blit(0, target, mip, 0, 0, 0, source, mip, 0, 0, 0, w, h, 1);
    std::vector<uint8_t> actual(expected.size() + 32, 0xcd);
    auto start = std::chrono::steady_clock::now();
    const uint32_t ready = bgfx::readTexture(target, actual.data() + 16, mip);
    while (bgfx::frame() < ready) {}
    elapsed += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    for (size_t i = 0; i < actual.size(); ++i) {
      const uint8_t wanted = i < 16 || i >= expected.size() + 16
                                 ? 0xcd : expected[i - 16];
      if (actual[i] != wanted) {
        fprintf(stderr, "FAIL: %ux%u frame %d byte %zu (%u != %u)\n",
                w, h, frame, i, actual[i], wanted);
        passed = false;
        break;
      }
    }
  }
  printf("Metal readback %ux%u format %d mipmapped %d: %.3f ms/frame\n",
         width, height, int(format), mipmapped, elapsed / 24);
  bgfx::destroy(source);
  bgfx::destroy(target);
  bgfx::frame();
  bgfx::frame();
  return passed;
}

int main() {
  @autoreleasepool {
    bgfx::renderFrame();
    bgfx::Init init;
    init.type = bgfx::RendererType::Metal;
    init.fallback = false;
    init.resolution.width = 0;
    init.resolution.height = 0;
    if (!bgfx::init(init)) return 1;
    bool passed = checkExternalTextureReuse();
    // Odd row widths, repeated GPU writes, format ordering, fallback mips,
    // destruction/reuse, and full-size export readbacks.
    for (auto format : {bgfx::TextureFormat::RGBA8, bgfx::TextureFormat::BGRA8}) {
      passed = checkReadback(1, 1, format, false) && passed;
      passed = checkReadback(257, 63, format, false) && passed;
      passed = checkReadback(256, 64, format, true) && passed;
      passed = checkReadback(2400, 1080, format, false) && passed;
    }
    bgfx::shutdown();
    return passed ? 0 : 1;
  }
}
