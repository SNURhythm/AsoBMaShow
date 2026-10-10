// Compile the real AAudio backend on the host; replace only the Android API boundary.
#define MA_SUPPORT_AAUDIO
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_AAUDIO
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#if !defined(__ANDROID__)
static int ma_android_sdk_version() { return 33; }
#endif
#include <miniaudio.h>

#include <cstdio>
#include <stdexcept>
#include <string>

struct AAudioStreamBuilder {
  int callbackFrames = 0, capacity = 0, sharing = 0;
  int usage = 0, performance = 0, sampleRate = 0;
};
struct AAudioStream { int unused; };

namespace {
AAudioStreamBuilder observed;
AAudioStream stream;
int burst = 192, capacity = 1536, requestedSize = 0, opens = 0, closes = 0, starts = 0;
int nativeRate = 48000, liveBuilders = 0;
bool rejectExclusive = false, rejectResize = false;
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
ma_aaudio_result_t create(ma_AAudioStreamBuilder** out) { ++liveBuilders; *out = reinterpret_cast<ma_AAudioStreamBuilder*>(new AAudioStreamBuilder); return 0; }
ma_aaudio_result_t destroy(ma_AAudioStreamBuilder* builder) { --liveBuilders; delete reinterpret_cast<AAudioStreamBuilder*>(builder); return 0; }
void ignore(ma_AAudioStreamBuilder*, int32_t) {}
void setCallback(ma_AAudioStreamBuilder* builder, int32_t n) { reinterpret_cast<AAudioStreamBuilder*>(builder)->callbackFrames = n; }
void setCapacity(ma_AAudioStreamBuilder* builder, int32_t n) { reinterpret_cast<AAudioStreamBuilder*>(builder)->capacity = n; }
void setSharing(ma_AAudioStreamBuilder* builder, int32_t n) { reinterpret_cast<AAudioStreamBuilder*>(builder)->sharing = n; }
void setUsage(ma_AAudioStreamBuilder* builder, int32_t n) { reinterpret_cast<AAudioStreamBuilder*>(builder)->usage = n; }
void setPerformance(ma_AAudioStreamBuilder* builder, int32_t n) { reinterpret_cast<AAudioStreamBuilder*>(builder)->performance = n; }
void setRate(ma_AAudioStreamBuilder* builder, int32_t n) { reinterpret_cast<AAudioStreamBuilder*>(builder)->sampleRate = n; }
void setDataCallback(ma_AAudioStreamBuilder*, ma_AAudioStream_dataCallback, void*) {}
void setErrorCallback(ma_AAudioStreamBuilder*, ma_AAudioStream_errorCallback, void*) {}
ma_aaudio_result_t open(ma_AAudioStreamBuilder* builder, ma_AAudioStream** out) {
  ++opens;
  observed = *reinterpret_cast<AAudioStreamBuilder*>(builder);
  if (rejectExclusive && reinterpret_cast<AAudioStreamBuilder*>(builder)->sharing == MA_AAUDIO_SHARING_MODE_EXCLUSIVE) return -1;
  *out = reinterpret_cast<ma_AAudioStream*>(&stream);
  return 0;
}
ma_aaudio_result_t close(ma_AAudioStream*) { ++closes; return 0; }
int32_t getFormat(ma_AAudioStream*) { return MA_AAUDIO_FORMAT_PCM_I16; }
int32_t getChannels(ma_AAudioStream*) { return 2; }
int32_t getRate(ma_AAudioStream*) { return nativeRate; }
int32_t start(ma_AAudioStream*) { ++starts; return MA_AAUDIO_OK; }
int32_t state(ma_AAudioStream*) { return MA_AAUDIO_STREAM_STATE_STARTED; }
int32_t getCapacity(ma_AAudioStream*) { return capacity; }
int32_t getCallback(ma_AAudioStream*) { return observed.callbackFrames; }
int32_t getBurst(ma_AAudioStream*) { return burst; }
int32_t setSize(ma_AAudioStream*, int32_t n) { requestedSize = n; return rejectResize ? -1 : n; }

ma_context context() {
  ma_context c{};
  c.backend = ma_backend_aaudio;
  c.allocationCallbacks = ma_allocation_callbacks_init_default();
  c.callbacks.onDeviceGetInfo = ma_device_get_info__aaudio;
  c.aaudio.AAudioStream_requestStart = (ma_proc)start;
  c.aaudio.AAudioStream_getState = (ma_proc)state;
  c.aaudio.AAudio_createStreamBuilder = (ma_proc)create;
  c.aaudio.AAudioStreamBuilder_delete = (ma_proc)destroy;
  c.aaudio.AAudioStreamBuilder_setDirection = (ma_proc)ignore;
  c.aaudio.AAudioStreamBuilder_setSharingMode = (ma_proc)setSharing;
  c.aaudio.AAudioStreamBuilder_setFormat = (ma_proc)ignore;
  c.aaudio.AAudioStreamBuilder_setChannelCount = (ma_proc)ignore;
  c.aaudio.AAudioStreamBuilder_setSampleRate = (ma_proc)setRate;
  c.aaudio.AAudioStreamBuilder_setBufferCapacityInFrames = (ma_proc)setCapacity;
  c.aaudio.AAudioStreamBuilder_setFramesPerDataCallback = (ma_proc)setCallback;
  c.aaudio.AAudioStreamBuilder_setUsage = (ma_proc)setUsage;
  c.aaudio.AAudioStreamBuilder_setPerformanceMode = (ma_proc)setPerformance;
  c.aaudio.AAudioStreamBuilder_setDataCallback = (ma_proc)setDataCallback;
  c.aaudio.AAudioStreamBuilder_setErrorCallback = (ma_proc)setErrorCallback;
  c.aaudio.AAudioStreamBuilder_openStream = (ma_proc)open;
  c.aaudio.AAudioStream_close = (ma_proc)close;
  c.aaudio.AAudioStream_getFormat = (ma_proc)getFormat;
  c.aaudio.AAudioStream_getChannelCount = (ma_proc)getChannels;
  c.aaudio.AAudioStream_getSampleRate = (ma_proc)getRate;
  c.aaudio.AAudioStream_getBufferCapacityInFrames = (ma_proc)getCapacity;
  c.aaudio.AAudioStream_getFramesPerDataCallback = (ma_proc)getCallback;
  c.aaudio.AAudioStream_getFramesPerBurst = (ma_proc)getBurst;
  c.aaudio.AAudioStream_setBufferSizeInFrames = (ma_proc)setSize;
  return c;
}
ma_device_descriptor descriptor() {
  ma_device_descriptor d{};
  d.format = ma_format_s16;
  d.channels = 2;
  d.periodCount = 3;
  return d;
}
void nativeCallbackIsNotPinnedToTenMilliseconds() {
  auto c = context();
  ma_device device{};
  device.pContext = &c;
  auto config = ma_device_config_init(ma_device_type_playback);
  config.noFixedSizedCallback = MA_TRUE;
  config.aaudio.bufferSizeInBursts = 2;
  auto d = descriptor();
  ma_AAudioStream* output = nullptr;
  require(ma_device_init_by_type__aaudio(&device, &config, ma_device_type_playback, &d, &output) == MA_SUCCESS, "native stream opens");
  require(observed.callbackFrames == 0, "native callback must not be pinned to miniaudio's 10ms default");
  require(observed.capacity == 0, "native capacity must be chosen by the device");
  require(requestedSize == 384, "initial output buffer must use two 192-frame hardware bursts");
  require(d.periodSizeInFrames == 192 && d.periodCount == 2, "descriptor must describe hardware bursts, not full capacity");
}
void resizeHandlesCapacityAndUnavailableApis() {
  auto c = context();
  ma_device device{};
  device.pContext = &c;
  auto config = ma_device_config_init(ma_device_type_playback);
  config.aaudio.bufferSizeInBursts = 2;
  auto check = [&](int expectedRequest, unsigned expectedPeriods) {
    requestedSize = 0;
    auto d = descriptor();
    ma_AAudioStream* output = nullptr;
    require(ma_device_init_by_type__aaudio(&device, &config, ma_device_type_playback, &d, &output) == MA_SUCCESS, "resize failure must not prevent playback");
    require(requestedSize == expectedRequest, "resize request must be bounded by native capacity");
    require(d.periodCount == expectedPeriods, "descriptor must retain usable period count");
  };
  capacity = 256;
  check(256, 2);
  capacity = 1536;
  rejectResize = true;
  check(384, 8);
  rejectResize = false;
  c.aaudio.AAudioStream_setBufferSizeInFrames = nullptr;
  check(0, 8);
  c.aaudio.AAudioStream_setBufferSizeInFrames = (ma_proc)setSize;
  config.aaudio.bufferSizeInBursts = UINT32_MAX;
  check(1536, 8);
  c.aaudio.AAudioStream_getFramesPerBurst = nullptr;
  check(0, 1);
  c.aaudio.AAudioStream_getFramesPerBurst = (ma_proc)getBurst;
  burst = 0;
  check(0, 1);
  burst = -1;
  check(0, 1);
  burst = INT32_MAX;
  check(1536, 1);
  burst = 192;
}
void exclusiveFailureRetriesShared() {
  auto c = context();
  ma_device device{};
  device.pContext = &c;
  auto config = ma_device_config_init(ma_device_type_playback);
  config.aaudio.bufferSizeInBursts = 2;
  config.aaudio.usage = ma_aaudio_usage_game;
  auto d = descriptor();
  d.shareMode = ma_share_mode_exclusive;
  ma_AAudioStream* output = nullptr;
  rejectExclusive = true;
  opens = 0;
  require(ma_device_init_by_type__aaudio(&device, &config, ma_device_type_playback, &d, &output) == MA_SUCCESS, "exclusive failure must fall back to shared playback");
  require(opens == 2 && observed.sharing == MA_AAUDIO_SHARING_MODE_SHARED, "retry shared once");
  require(observed.usage == MA_AAUDIO_USAGE_GAME && observed.performance == MA_AAUDIO_PERFORMANCE_MODE_LOW_LATENCY, "fallback must retain game and low latency mode");
  require(observed.callbackFrames == 0 && requestedSize == 384, "fallback must retain native callback and burst sizing");
  rejectExclusive = false;
}
void rerouteReappliesBurstPolicyAndPreservesRunState() {
  auto c = context();
  ma_device device{};
  device.pContext = &c;
  device.type = ma_device_type_playback;
  device.playback.format = ma_format_s16;
  device.playback.channels = 2;
  device.playback.shareMode = ma_share_mode_exclusive;
  device.noFixedSizedCallback = MA_TRUE;
  auto config = ma_device_config_init(ma_device_type_playback);
  config.aaudio.bufferSizeInBursts = 2;
  config.aaudio.usage = ma_aaudio_usage_game;
  config.aaudio.enableCompatibilityWorkarounds = MA_TRUE;
  auto d = descriptor();
  d.shareMode = ma_share_mode_exclusive;
  require(ma_device_init__aaudio(&device, &config, &d, nullptr) == MA_SUCCESS, "initial route opens");
  const auto initialized = ma_device_post_init(&device, ma_device_type_playback, &d, nullptr);
  require(initialized == MA_SUCCESS, "initial route conversion initializes");
  ma_device__set_state(&device, ma_device_state_started);
  burst = 240;
  nativeRate = 44100;
  requestedSize = 0;
  rejectExclusive = true;
  starts = closes = 0;
  require(ma_device_reinit__aaudio(&device, ma_device_type_playback) == MA_SUCCESS, "reroute must preserve shared fallback");
  require(requestedSize == 480, "reroute must resize to the new hardware burst");
  require(observed.callbackFrames == 0 && observed.capacity == 0 && observed.sampleRate == 0, "reroute must renegotiate native format timing");
  require(device.sampleRate == 48000 && device.playback.internalSampleRate == 44100, "reroute must preserve client rate while converting to native rate");
  require(starts == 1 && closes == 1, "running reroute must close old stream and restart once");
  require(observed.usage == MA_AAUDIO_USAGE_GAME, "reroute must preserve game usage");
  ma_device__set_state(&device, ma_device_state_stopped);
  require(ma_device_reinit__aaudio(&device, ma_device_type_playback) == MA_SUCCESS, "stopped reroute opens");
  require(starts == 1, "stopped reroute must not start audio");
  ma_device_uninit__aaudio(&device);
  ma_data_converter_uninit(&device.playback.converter, nullptr);
  rejectExclusive = false;
  nativeRate = 48000;
  burst = 192;
}
void defaultAndCaptureConfigurationStayUnchanged() {
  auto c = context();
  ma_device device{};
  device.pContext = &c;
  auto config = ma_device_config_init(ma_device_type_playback);
  auto d = descriptor();
  ma_AAudioStream* output = nullptr;
  requestedSize = 0;
  require(ma_device_init_by_type__aaudio(&device, &config, ma_device_type_playback, &d, &output) == MA_SUCCESS, "default opens");
  require(observed.callbackFrames == 480 && observed.capacity == 1440 && requestedSize == 0, "non-opted-in AAudio must retain defaults");
  config.aaudio.bufferSizeInBursts = 2;
  d = descriptor();
  require(ma_device_init_by_type__aaudio(&device, &config, ma_device_type_capture, &d, &output) == MA_SUCCESS, "capture opens");
  require(observed.callbackFrames == 480 && observed.capacity == 1440 && requestedSize == 0, "playback burst policy must not alter capture");
}
}
int main() {
  try {
    nativeCallbackIsNotPinnedToTenMilliseconds();
    resizeHandlesCapacityAndUnavailableApis();
    exclusiveFailureRetriesShared();
    rerouteReappliesBurstPolicyAndPreservesRunState();
    defaultAndCaptureConfigurationStayUnchanged();
    require(liveBuilders == 0, "all builders must be released including failed exclusive attempts");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  return 0;
}
