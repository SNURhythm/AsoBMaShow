#import <CoreVideo/CoreVideo.h>
#include "video/HardwareVideoFrame.h"
#include <bgfx/platform.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
extern "C" {
#include <libavutil/frame.h>
}

static bgfx::ShaderHandle shader(const char *name) {
  std::ifstream file(std::string(ASOBMASHOW_SOURCE_DIR) + "/shaders/metal/" + name, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
  if (bytes.empty()) return BGFX_INVALID_HANDLE;
  return bgfx::createShader(bgfx::copy(bytes.data(), static_cast<uint32_t>(bytes.size())));
}

static bool checkPixels() {
  const auto vertex = shader("vs_yuvrgb.bin");
  const auto fragment = shader("fs_video_nv12.bin");
  if (!bgfx::isValid(vertex) || !bgfx::isValid(fragment)) {
    if (bgfx::isValid(vertex)) bgfx::destroy(vertex);
    std::fprintf(stderr, "FAIL: Native video rendering shader is available\n");
    return false;
  }
  const auto program = bgfx::createProgram(vertex, fragment, true);
  const auto ySampler = bgfx::createUniform("s_texY", bgfx::UniformType::Sampler);
  const auto uvSampler = bgfx::createUniform("s_texU", bgfx::UniformType::Sampler);
  const std::array uniforms{
      bgfx::createUniform("u_videoColor0", bgfx::UniformType::Vec4),
      bgfx::createUniform("u_videoColor1", bgfx::UniformType::Vec4),
      bgfx::createUniform("u_videoColor2", bgfx::UniformType::Vec4)};
  const auto output = bgfx::createTexture2D(32, 24, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  const auto readback = bgfx::createTexture2D(32, 24, false, 1, bgfx::TextureFormat::RGBA8,
      BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
  const auto framebuffer = bgfx::createFrameBuffer(1, &output, false);
  bgfx::setViewFrameBuffer(0, framebuffer);
  bgfx::setViewRect(0, 0, 0, 32, 24);
  bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x123456ff);
  struct Vertex { float x, y, z, u, v; };
  bgfx::VertexLayout layout;
  layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
  struct Case { bool full; AVColorSpace space; uint8_t y, u, v; std::array<int, 3> rgb; };
  const std::array cases{
      Case{false, AVCOL_SPC_BT709, 16, 128, 128, {0, 0, 0}},
      Case{false, AVCOL_SPC_BT709, 235, 128, 128, {255, 255, 255}},
      Case{false, AVCOL_SPC_BT709, 63, 102, 240, {255, 1, 0}},
      Case{false, AVCOL_SPC_SMPTE170M, 81, 90, 240, {254, 0, 0}},
      Case{true, AVCOL_SPC_BT709, 0, 128, 128, {0, 0, 0}},
      Case{true, AVCOL_SPC_BT709, 255, 128, 128, {255, 255, 255}}};
  bool passed = true;
  for (const auto &test : cases) {
    const NSDictionary *attributes = @{
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (id)kCVPixelBufferWidthKey: @32, (id)kCVPixelBufferHeightKey: @24,
        (id)kCVPixelBufferPixelFormatTypeKey: @(test.full ?
            kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    CVPixelBufferPoolRef pool = nullptr;
    CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (CFDictionaryRef)attributes, &pool);
    const NSDictionary *threshold = @{(id)kCVPixelBufferPoolAllocationThresholdKey: @1};
    CVPixelBufferRef pixels = nullptr;
    CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, pool,
        (CFDictionaryRef)threshold, &pixels);
    CVPixelBufferLockBaseAddress(pixels, 0);
    for (size_t row = 0; row < 24; ++row)
      std::memset((uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixels, 0) + row * CVPixelBufferGetBytesPerRowOfPlane(pixels, 0), test.y, 32);
    for (size_t row = 0; row < 12; ++row) {
      auto *chroma = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixels, 1) + row * CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
      for (size_t col = 0; col < 32; col += 2) { chroma[col] = test.u; chroma[col + 1] = test.v; }
    }
    CVPixelBufferUnlockBaseAddress(pixels, 0);
    AVFrame frame{};
    frame.format = AV_PIX_FMT_VIDEOTOOLBOX; frame.width = 32; frame.height = 24;
    frame.colorspace = test.space;
    frame.data[3] = reinterpret_cast<uint8_t *>(pixels);
    video::HardwareVideoTextures textures;
    const bool imported = video::importHardwareVideoFrame(frame, textures);
    CVPixelBufferRelease(pixels); // The decoder is allowed to release this now.
    if (!imported) { CFRelease(pool); passed = false; break; }
    CVPixelBufferRef reused = nullptr;
    const auto pending = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault,
        pool, (CFDictionaryRef)threshold, &reused);
    if (pending != kCVReturnWouldExceedAllocationThreshold) {
      std::fprintf(stderr, "FAIL: decoder pool reused a surface still queued for rendering\n");
      passed = false;
    }
    if (reused) { CVPixelBufferRelease(reused); reused = nullptr; }
    bgfx::TransientVertexBuffer vertices;
    bgfx::allocTransientVertexBuffer(&vertices, 3, layout);
    const Vertex triangle[]{{-1, -1, 0, 0, 1}, {3, -1, 0, 2, 1}, {-1, 3, 0, 0, -1}};
    std::memcpy(vertices.data, triangle, sizeof(triangle));
    bgfx::setVertexBuffer(0, &vertices);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::setTexture(0, ySampler, textures.planes[0]);
    bgfx::setTexture(1, uvSampler, textures.planes[1]);
    for (size_t row = 0; row < 3; ++row) bgfx::setUniform(uniforms[row], textures.colorRows[row].data());
    bgfx::submit(0, program);
    // Retire the handles even before the render thread consumes the draw.
    video::destroyHardwareVideoTextures(textures);
    bgfx::blit(1, readback, 0, 0, output);
    std::vector<uint8_t> actual(32 * 24 * 4);
    const auto ready = bgfx::readTexture(readback, actual.data());
    while (bgfx::frame() < ready) {}
    const auto retired = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault,
        pool, (CFDictionaryRef)threshold, &reused);
    if (retired != kCVReturnSuccess) {
      std::fprintf(stderr, "FAIL: completed surface did not return to decoder pool\n");
      passed = false;
    }
    if (reused) CVPixelBufferRelease(reused);
    CFRelease(pool);
    for (size_t pixel = 0; pixel < 32 * 24; ++pixel) {
      for (size_t channel = 0; channel < 3; ++channel) {
        if (std::abs(int(actual[pixel * 4 + channel]) - test.rgb[channel]) > 2) {
          std::fprintf(stderr, "FAIL: native YUV color %u/%u/%u channel %zu: %u expected %d\n",
              test.y, test.u, test.v, channel, actual[pixel * 4 + channel], test.rgb[channel]);
          passed = false; break;
        }
      }
      if (!passed) break;
    }
    if (!passed) break;
  }
  bgfx::setViewFrameBuffer(0, BGFX_INVALID_HANDLE);
  bgfx::destroy(framebuffer); bgfx::destroy(output); bgfx::destroy(readback);
  bgfx::destroy(program); bgfx::destroy(ySampler); bgfx::destroy(uvSampler);
  for (auto uniform : uniforms) bgfx::destroy(uniform);
  return passed;
}

int main(int argc, char **) {
  @autoreleasepool {
    if (argc == 1) bgfx::renderFrame();
    bgfx::Init init;
    init.type = bgfx::RendererType::Metal;
    init.fallback = false;
    init.resolution.width = init.resolution.height = 0;
    if (!bgfx::init(init)) return 1;
    const NSDictionary *attributes = @{
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    CVPixelBufferRef pixels = nullptr;
    const auto status = CVPixelBufferCreate(kCFAllocatorDefault, 32, 24,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        (CFDictionaryRef)attributes, &pixels);
    AVFrame frame{};
    frame.format = AV_PIX_FMT_VIDEOTOOLBOX;
    frame.width = 32; frame.height = 24;
    frame.color_range = AVCOL_RANGE_MPEG;
    frame.colorspace = AVCOL_SPC_BT709;
    frame.data[3] = reinterpret_cast<uint8_t *>(pixels);
    video::HardwareVideoTextures textures;
    const bool imported = status == kCVReturnSuccess &&
        video::importHardwareVideoFrame(frame, textures);
    CVPixelBufferRelease(pixels);
    bool passed = imported && bgfx::isValid(textures.planes[0]) &&
                  bgfx::isValid(textures.planes[1]);
    if (!passed) std::fprintf(stderr, "FAIL: Native NV12 surface imports without a CPU upload\n");
    video::destroyHardwareVideoTextures(textures);
    passed = checkPixels() && passed;
    bgfx::frame();
    bgfx::frame();
    bgfx::shutdown();
    return passed ? 0 : 1;
  }
}
