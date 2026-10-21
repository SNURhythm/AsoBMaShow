#include "HardwareVideoFrame.h"
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <objc/runtime.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
bool asobmashowMetalRegisterOwnedTexture(uintptr_t texture);
void asobmashowMetalCancelOwnedTexture(uintptr_t texture);
uintptr_t asobmashowMetalRetainDevice();
}

// Attach to a separate Metal texture view, never to the CVMetalTexture's own
// texture: the latter would form a retain cycle. Metal command buffers retain
// the view, which in turn keeps both the surface and its CoreVideo mapping live.
@interface ASOVideoSurfaceOwner : NSObject {
@public
  CVPixelBufferRef pixels;
  CVMetalTextureRef mappings[2];
}
@end
@implementation ASOVideoSurfaceOwner
- (void)dealloc {
  for (auto mapping : mappings) if (mapping) CFRelease(mapping);
  if (pixels) CVPixelBufferRelease(pixels);
}
@end

namespace {
char ownerKey;
struct TextureCache {
  id<MTLDevice> device = nil;
  CVMetalTextureCacheRef value = nullptr;
  ~TextureCache() { if (value) CFRelease(value); }
  bool use(id<MTLDevice> next) {
    if (device == next && value) return true;
    if (value) { CFRelease(value); value = nullptr; }
    device = next;
    return CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &value) == kCVReturnSuccess;
  }
};
struct SurfaceViews {
  id<MTLTexture> planes[2] = {nil, nil};
};

CMVideoFormatDescriptionRef decoderDescription(const AVCodecContext &context) {
  const bool hevc = context.codec_id == AV_CODEC_ID_HEVC;
  CMVideoFormatDescriptionRef description = nullptr;
  if (context.extradata[0] == 1) {
    NSData *configuration = [NSData dataWithBytes:context.extradata length:context.extradata_size];
    NSDictionary *extensions = @{
        (__bridge id)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms:
            @{hevc ? @"hvcC" : @"avcC": configuration}};
    CMVideoFormatDescriptionCreate(kCFAllocatorDefault, hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
        context.width, context.height, (__bridge CFDictionaryRef)extensions, &description);
    return description;
  }
  // FFmpeg encoders expose Annex B parameter sets before MP4 muxing, while
  // demuxed MP4 uses the configuration records handled above.
  const auto *bytes = context.extradata;
  const auto size = static_cast<size_t>(context.extradata_size);
  std::array<const uint8_t *, 3> sets{};
  std::array<size_t, 3> lengths{};
  const auto startCode = [&](size_t offset) {
    for (; offset + 3 <= size; ++offset)
      if (bytes[offset] == 0 && bytes[offset + 1] == 0 && bytes[offset + 2] == 1) return offset;
    return size;
  };
  for (size_t start = startCode(0); start < size;) {
    const auto begin = start + 3;
    const auto next = startCode(begin);
    auto end = next;
    while (end > begin && bytes[end - 1] == 0) --end;
    if (end > begin) {
      const int type = hevc ? (bytes[begin] >> 1) & 63 : bytes[begin] & 31;
      const int index = type - (hevc ? 32 : 7);
      if (index >= 0 && index < (hevc ? 3 : 2)) {
        sets[index] = bytes + begin; lengths[index] = end - begin;
      }
    }
    start = next;
  }
  const size_t count = hevc ? 3 : 2;
  for (size_t i = 0; i < count; ++i) if (!sets[i]) return nullptr;
  if (hevc)
    CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, count,
        sets.data(), lengths.data(), 4, nullptr, &description);
  else
    CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, count,
        sets.data(), lengths.data(), 4, &description);
  return description;
}

bool makeViews(const AVFrame &frame, id<MTLDevice> device, SurfaceViews &out,
               std::array<std::array<float, 4>, 3> &rows) {
  if (frame.format != AV_PIX_FMT_VIDEOTOOLBOX || !frame.data[3] ||
      frame.width <= 0 || frame.height <= 0 || frame.width > UINT16_MAX || frame.height > UINT16_MAX)
    return false;
  const auto pixels = reinterpret_cast<CVPixelBufferRef>(frame.data[3]);
  const auto format = CVPixelBufferGetPixelFormatType(pixels);
  const bool fullRange = format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
  if ((!fullRange && format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) ||
      CVPixelBufferGetPlaneCount(pixels) != 2 ||
      CVPixelBufferGetWidth(pixels) != static_cast<size_t>(frame.width) ||
      CVPixelBufferGetHeight(pixels) != static_cast<size_t>(frame.height)) return false;

  float kr = 0.299F, kb = 0.114F;
  if (frame.colorspace == AVCOL_SPC_BT709) { kr = 0.2126F; kb = 0.0722F; }
  else if (frame.colorspace == AVCOL_SPC_BT2020_NCL) { kr = 0.2627F; kb = 0.0593F; }
  else if (frame.colorspace != AVCOL_SPC_UNSPECIFIED && frame.colorspace != AVCOL_SPC_BT470BG &&
           frame.colorspace != AVCOL_SPC_SMPTE170M) return false;
  if (frame.color_trc == AVCOL_TRC_SMPTE2084 || frame.color_trc == AVCOL_TRC_ARIB_STD_B67) return false;
  const float ys = fullRange ? 1.0F : 255.0F / 219.0F;
  const float cs = fullRange ? 1.0F : 255.0F / 224.0F;
  const float yo = fullRange ? 0.0F : 16.0F / 255.0F;
  const float co = 128.0F / 255.0F;
  const float rv = 2.0F * (1.0F - kr) * cs;
  const float bu = 2.0F * (1.0F - kb) * cs;
  const float gu = -2.0F * kb * (1.0F - kb) / (1.0F - kr - kb) * cs;
  const float gv = -2.0F * kr * (1.0F - kr) / (1.0F - kr - kb) * cs;
  rows = {{{ys, 0, rv, -ys * yo - rv * co},
           {ys, gu, gv, -ys * yo - (gu + gv) * co},
           {ys, bu, 0, -ys * yo - bu * co}}};

  thread_local TextureCache cache;
  if (!cache.use(device)) return false;
  ASOVideoSurfaceOwner *owner = [[ASOVideoSurfaceOwner alloc] init];
  if (!owner) return false;
  owner->pixels = CVPixelBufferRetain(pixels);
  bool success = true;
  for (size_t plane = 0; plane < 2; ++plane) {
    const auto pixelFormat = plane == 0 ? MTLPixelFormatR8Unorm : MTLPixelFormatRG8Unorm;
    const auto result = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache.value,
        pixels, nullptr, pixelFormat, CVPixelBufferGetWidthOfPlane(pixels, plane),
        CVPixelBufferGetHeightOfPlane(pixels, plane), plane, &owner->mappings[plane]);
    if (result != kCVReturnSuccess) { success = false; break; }
    out.planes[plane] = [CVMetalTextureGetTexture(owner->mappings[plane]) newTextureViewWithPixelFormat:pixelFormat];
    if (!out.planes[plane]) { success = false; break; }
    objc_setAssociatedObject(out.planes[plane], &ownerKey, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  CVMetalTextureCacheFlush(cache.value, 0);
  return success;
}
}

namespace video {
bool hardwareVideoRenderingAvailable() {
  return bgfx::getRendererType() == bgfx::RendererType::Metal &&
      (bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_EXTERNAL);
}
bool importHardwareVideoFrame(const AVFrame &frame, HardwareVideoTextures &out) {
  if (!hardwareVideoRenderingAvailable()) return false;
  @autoreleasepool {
    id<MTLDevice> device = (__bridge_transfer id<MTLDevice>)(void *)asobmashowMetalRetainDevice();
    SurfaceViews views;
    HardwareVideoTextures imported;
    const bool ready = makeViews(frame, device, views, imported.colorRows);
    if (!ready) return false;
    for (size_t plane = 0; plane < 2; ++plane) {
      const auto native = reinterpret_cast<uintptr_t>((__bridge void *)views.planes[plane]);
      if (!asobmashowMetalRegisterOwnedTexture(native)) {
        destroyHardwareVideoTextures(imported); return false;
      }
      imported.planes[plane] = bgfx::createTexture2D(
          static_cast<uint16_t>(views.planes[plane].width),
          static_cast<uint16_t>(views.planes[plane].height), false, 1,
          plane == 0 ? bgfx::TextureFormat::R8 : bgfx::TextureFormat::RG8,
          BGFX_SAMPLER_UVW_CLAMP, nullptr, native);
      if (!bgfx::isValid(imported.planes[plane])) {
        asobmashowMetalCancelOwnedTexture(native);
        destroyHardwareVideoTextures(imported); return false;
      }
    }
    destroyHardwareVideoTextures(out);
    out = imported;
    return true;
  }
}
bool probeHardwareVideoFrame(const AVFrame &frame) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return false;
    SurfaceViews views;
    std::array<std::array<float, 4>, 3> rows;
    const bool result = makeViews(frame, device, views, rows);
    return result;
  }
}
bool probeHardwareVideoDecoder(const AVCodecContext &context) {
  // VideoToolbox can also return native surfaces from a software decoder.
  // Verify hardware support for this exact configuration, rather than infer
  // it from AV_PIX_FMT_VIDEOTOOLBOX or the device model alone.
  if ((context.codec_id != AV_CODEC_ID_H264 && context.codec_id != AV_CODEC_ID_HEVC) ||
      !context.extradata || context.extradata_size < 7)
    return false;
  @autoreleasepool {
    const bool hevc = context.codec_id == AV_CODEC_ID_HEVC;
    const CMVideoCodecType codec = hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
    if (!VTIsHardwareDecodeSupported(codec)) return false;
    CMVideoFormatDescriptionRef description = decoderDescription(context);
    if (!description) return false;
    NSDictionary *specification = @{
        (__bridge id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES};
    VTDecompressionSessionRef session = nullptr;
    const auto status = VTDecompressionSessionCreate(kCFAllocatorDefault, description,
        (__bridge CFDictionaryRef)specification, nullptr, nullptr, &session);
    CFRelease(description);
    if (status != noErr) return false;
    CFTypeRef usingHardware = nullptr;
    const auto property = VTSessionCopyProperty(session,
        kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
        kCFAllocatorDefault, &usingHardware);
    const bool result = property == noErr && usingHardware == kCFBooleanTrue;
    if (usingHardware) CFRelease(usingHardware);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    return result;
  }
}
}
