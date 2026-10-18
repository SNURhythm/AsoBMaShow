#include "StorageVideoEncoder.h"
#include "StorageTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace storage_management {
namespace {
std::string errorText(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  av_strerror(code, buffer.data(), buffer.size());
  return buffer.data();
}

#if defined(__ANDROID__)
struct AndroidLocalFrame {
  JNIEnv *env;
  ~AndroidLocalFrame() {
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
  }
};

std::string javaString(JNIEnv *env, jstring value) {
  if (!value || env->ExceptionCheck()) return {};
  const char *chars = env->GetStringUTFChars(value, nullptr);
  if (!chars) return {};
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

StorageVideoEncoderCandidate androidCandidate(VideoCodec codec, int width, int height,
                                               int fps, int bitrate) {
  StorageVideoEncoderCandidate candidate;
  // FFmpeg pads byte-buffer AVC/HEVC inputs to 16 pixels before configuring
  // MediaCodec, then restores display cropping in the encoded bitstream.
  if (codec != VideoCodec::Av1) {
    if (width > std::numeric_limits<int>::max() - 15 ||
        height > std::numeric_limits<int>::max() - 15) return candidate;
    width = (width + 15) & ~15;
    height = (height + 15) & ~15;
  }
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  if (!env) return candidate;
  if (env->PushLocalFrame(12) < 0) {
    env->ExceptionClear();
    return candidate;
  }
  AndroidLocalFrame local{env};
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!activity || env->ExceptionCheck()) return candidate;
  auto activityClass = env->GetObjectClass(activity);
  if (!activityClass || env->ExceptionCheck()) return candidate;
  auto find = env->GetMethodID(activityClass, "findStorageVideoEncoder",
                              "(Ljava/lang/String;IIII)Ljava/lang/String;");
  if (!find || env->ExceptionCheck()) return candidate;
  auto pixelFormat = env->GetMethodID(activityClass, "getStorageVideoEncoderPixelFormat",
                                     "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  if (!pixelFormat || env->ExceptionCheck()) return candidate;
  const char *mime = codec == VideoCodec::H264 ? "video/avc" :
                     codec == VideoCodec::Hevc ? "video/hevc" : "video/av01";
  auto mimeString = env->NewStringUTF(mime);
  if (!mimeString || env->ExceptionCheck()) return candidate;
  auto name = static_cast<jstring>(env->CallObjectMethod(
      activity, find, mimeString, width, height, fps, bitrate));
  auto deviceName = javaString(env, name);
  if (deviceName.empty() || env->ExceptionCheck()) return candidate;
  auto format = static_cast<jstring>(env->CallObjectMethod(activity, pixelFormat, name, mimeString));
  const auto formatName = javaString(env, format);
  if (env->ExceptionCheck() || (formatName != "nv12" && formatName != "yuv420p")) return candidate;
  candidate.name = codec == VideoCodec::H264 ? "h264_mediacodec" :
                   codec == VideoCodec::Hevc ? "hevc_mediacodec" : "av1_mediacodec";
  candidate.deviceName = std::move(deviceName);
  candidate.hardware = true;
  candidate.pixelFormat = formatName == "nv12" ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  return candidate;
}
#endif
} // namespace

AVCodecID StorageVideoCodecId(VideoCodec codec) {
  switch (codec) {
  case VideoCodec::H264: return AV_CODEC_ID_H264;
  case VideoCodec::Hevc: return AV_CODEC_ID_HEVC;
  case VideoCodec::Av1: return AV_CODEC_ID_AV1;
  }
  return AV_CODEC_ID_NONE;
}

int StorageVideoEncoderBitrate(VideoCodec codec, VideoQuality quality,
                               int width, int height, int fps) {
  if (width <= 0 || height <= 0 || fps <= 0) return 0;
  // A bounded bitrate target for backends without a portable constant-quality
  // control. These quality levels are not claimed to equal x264 CRF values.
  const double density = quality == VideoQuality::High ? 0.20 :
                         quality == VideoQuality::Smaller ? 0.075 : 0.12;
  const double efficiency = codec == VideoCodec::H264 ? 1.0 :
                            codec == VideoCodec::Hevc ? 0.75 : 0.60;
  const double rate = double(width) * height * fps * density * efficiency;
  return static_cast<int>(std::clamp(rate, 256000.0, 120000000.0));
}

std::vector<StorageVideoEncoderCandidate> StorageVideoEncoderCandidates(
    VideoCodec codec, int width, int height, int fps, int bitrate) {
  std::vector<StorageVideoEncoderCandidate> result;
  if (StorageVideoCodecId(codec) == AV_CODEC_ID_NONE || width <= 0 || height <= 0 ||
      fps <= 0 || bitrate <= 0) return result;
  auto add = [&](const std::string &name, bool hardware) {
    const auto encoder = avcodec_find_encoder_by_name(name.c_str());
    if (encoder && encoder->id == StorageVideoCodecId(codec))
      result.push_back({name, {}, hardware, hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P});
  };
  const std::string prefix = codec == VideoCodec::H264 ? "h264" :
                             codec == VideoCodec::Hevc ? "hevc" : "av1";
#if defined(__ANDROID__)
  auto candidate = androidCandidate(codec, width, height, fps, bitrate);
  if (!candidate.name.empty() && avcodec_find_encoder_by_name(candidate.name.c_str()))
    result.push_back(std::move(candidate));
#elif defined(__APPLE__)
  add(prefix + "_videotoolbox", true);
#else
  add(prefix + "_nvenc", true);
  add(prefix + "_qsv", true);
  add(prefix + "_amf", true);
#if defined(_WIN32)
  add(prefix + "_mf", true);
#endif
#endif
  if (codec == VideoCodec::H264) add("libx264", false);
  return result;
}

bool ConfigureStorageVideoEncoder(AVCodecContext *context,
                                  const StorageVideoEncoderCandidate &candidate,
                                  const StoragePolicy &policy, std::string &reason) {
  reason.clear();
  const auto codec = avcodec_find_encoder_by_name(candidate.name.c_str());
  if (!context || !codec || codec->id != StorageVideoCodecId(policy.videoCodec) ||
      context->width <= 0 || context->height <= 0 || context->framerate.num <= 0 ||
      context->framerate.den <= 0 || context->time_base.num <= 0 || context->time_base.den <= 0) {
    reason = "Video encoder or its geometry/timing configuration is unavailable";
    return false;
  }
  const auto fps = (std::int64_t(context->framerate.num) + context->framerate.den - 1) /
                   context->framerate.den;
  if (fps > std::numeric_limits<int>::max() / 2) {
    reason = "Video frame rate exceeds encoder limits";
    return false;
  }
  context->pix_fmt = candidate.pixelFormat;
  context->max_b_frames = 0;
  context->gop_size = static_cast<int>(fps * 2);
  context->thread_count = StorageExecutionLimits::videoThreads;
  context->bit_rate = StorageVideoEncoderBitrate(policy.videoCodec, policy.videoQuality,
                                                context->width, context->height, static_cast<int>(fps));
  auto option = [&](const char *name, const std::string &value) {
    const int code = av_opt_set(context->priv_data, name, value.c_str(), 0);
    if (code < 0) reason = candidate.name + ": " + name + ": " + errorText(code);
    return code >= 0;
  };
  const auto &name = candidate.name;
  if (name == "libx264") {
    context->bit_rate = 0;
    if (!option("preset", "fast") || !option("crf", std::to_string(VideoCrf(policy.videoQuality)))) return false;
  } else if (name.find("videotoolbox") != std::string::npos) {
    if (!option("allow_sw", "0") || !option("require_sw", "0") ||
        !option("realtime", "0") ||
        !option("profile", policy.videoCodec == VideoCodec::H264 ? "high" : "main")) return false;
#if defined(__APPLE__) && !TARGET_OS_IPHONE && defined(__aarch64__)
    context->flags |= AV_CODEC_FLAG_QSCALE;
    context->global_quality = FF_QP2LAMBDA * (policy.videoQuality == VideoQuality::High ? 75 :
        policy.videoQuality == VideoQuality::Smaller ? 45 : 60);
#endif
  } else if (name.find("nvenc") != std::string::npos) {
    if (!option("preset", "p4") || !option("rc", "vbr")) return false;
  } else if (name.find("amf") != std::string::npos) {
    context->rc_max_rate = context->bit_rate * 2;
    if (!option("usage", "transcoding") || !option("quality", "balanced") ||
        !option("rc", "vbr_peak")) return false;
  } else if (name.find("qsv") != std::string::npos) {
    AVDictionary *options = nullptr;
#if defined(_WIN32)
    av_dict_set(&options, "child_device_type", "d3d11va", 0);
#endif
    const int code = av_hwdevice_ctx_create(&context->hw_device_ctx, AV_HWDEVICE_TYPE_QSV,
                                            "hw_any", options, 0);
    av_dict_free(&options);
    if (code < 0) {
      reason = "Create hardware-only QSV device: " + errorText(code);
      return false;
    }
    if (!option("preset", "medium")) return false;
  } else if (name.find("_mf") != std::string::npos) {
    if (!option("hw_encoding", "1")) return false;
  } else if (name.find("mediacodec") != std::string::npos) {
    if (candidate.deviceName.empty()) {
      reason = "No verified Android hardware encoder was selected";
      return false;
    }
    if (!option("codec_name", candidate.deviceName) || !option("ndk_codec", "1") ||
        !option("bitrate_mode", "vbr")) return false;
  } else {
    reason = "Unsupported storage video encoder";
    return false;
  }
  const int code = avcodec_open2(context, codec, nullptr);
  if (code < 0) reason = "Open " + candidate.name + ": " + errorText(code);
  return code >= 0;
}
} // namespace storage_management
