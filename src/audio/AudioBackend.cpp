#include "AudioBackend.h"

#include "SelectAudioDiagnostics.h"

#include "../targets.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if TARGET_OS_DESKTOP || TARGET_OS_LINUX
#include <portaudio.h>
#endif
#if TARGET_OS_WINDOWS
#include <pa_asio.h>
#endif

namespace audio {

namespace {

bool nativeBufferCanUseFrames(const NativeBufferFrameLimits &limits,
                              std::uint32_t frames) {
  if (frames == 0) {
    return true;
  }
  if (limits.minimum == 0 || limits.maximum < limits.minimum ||
      limits.granularity < -1) {
    return false;
  }
  if (limits.granularity == 0) {
    return limits.preferred >= limits.minimum &&
           limits.preferred <= limits.maximum &&
           limits.preferred % frames == 0;
  }
  if (limits.granularity == -1) {
    for (std::uint64_t nativeFrames = limits.minimum;
         nativeFrames <= limits.maximum; nativeFrames *= 2) {
      if (nativeFrames % frames == 0) {
        return true;
      }
      if (nativeFrames == 0 || nativeFrames > limits.maximum / 2) {
        break;
      }
    }
    return false;
  }

  const std::uint64_t step =
      static_cast<std::uint32_t>(limits.granularity);
  for (std::uint64_t nativeFrames = limits.minimum;
       nativeFrames <= limits.maximum; nativeFrames += step) {
    if (nativeFrames % frames == 0) {
      return true;
    }
    if (nativeFrames > limits.maximum -
                           std::min<std::uint64_t>(step, limits.maximum)) {
      break;
    }
  }
  return false;
}

} // namespace

std::vector<std::uint32_t>
SelectPortAudioBufferFrameOptions(
    std::span<const std::uint32_t> candidates,
    std::optional<NativeBufferFrameLimits> nativeLimits) {
  std::vector<std::uint32_t> supported;
  supported.reserve(candidates.size());
  for (const std::uint32_t frames : candidates) {
    if (!nativeLimits.has_value() ||
        nativeBufferCanUseFrames(*nativeLimits, frames)) {
      supported.push_back(frames);
    }
  }
  return supported;
}

namespace {

constexpr int kOutputChannels = 2;

void fillSilence(void *output, std::uint32_t frameCount, int channels) {
  if (output == nullptr || channels <= 0) {
    return;
  }
  std::fill_n(static_cast<std::int16_t *>(output),
              static_cast<std::size_t>(frameCount) *
                  static_cast<std::size_t>(channels),
              0);
}

class MiniaudioStream final : public IBackend {
public:
  MiniaudioStream(const StreamRequest &request, RenderCallback renderCallback,
                  void *renderUserData, std::string &errorMessage)
      : request_(request), renderCallback_(renderCallback),
        renderUserData_(renderUserData) {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = kOutputChannels;
    config.sampleRate = 0;
    config.dataCallback = &MiniaudioStream::dataCallback;
    config.pUserData = this;

#if TARGET_OS_IPHONE
    ma_context_config contextConfig = ma_context_config_init();
    // AVAudioSessionCategoryPlayback ignores the iPad's Silent/Mute switch, so
    // select SEs, BGM, previews, and gameplay keysounds are always audible.
    // The previous Ambient category (like SoloAmbient) is muted whenever the
    // device's ringer/side switch or Control Center mute is on, which the select
    // screen's sounds (and the looping BGM) depend on.
    contextConfig.coreaudio.sessionCategory = ma_ios_session_category_playback;
    contextConfig.coreaudio.sessionCategoryOptions =
        ma_ios_session_category_option_mix_with_others;
    const ma_result result =
        ma_device_init_ex(nullptr, 0, &contextConfig, &config, &device_);
#else
    const ma_result result = ma_device_init(nullptr, &config, &device_);
#endif
    audio::diag::SelectAudioLog("miniaudio init result=" +
                                std::string(ma_result_description(result)));
    if (result != MA_SUCCESS) {
      errorMessage = std::string("Miniaudio device initialization failed: ") +
                     ma_result_description(result);
      return;
    }
    initialized_ = true;
    state_.request = request_;
    state_.effectiveSampleRate = device_.sampleRate;
    state_.effectiveBufferFrames = device_.playback.internalPeriodSizeInFrames;
    if (state_.effectiveSampleRate != 0) {
      state_.effectiveLatencyMs =
          1000.0 * state_.effectiveBufferFrames / state_.effectiveSampleRate;
    }
  }

  ~MiniaudioStream() override {
    if (initialized_) {
      ma_device_uninit(&device_);
    }
  }

  [[nodiscard]] bool valid() const { return initialized_; }

  bool start(std::string &errorMessage) override {
    if (!initialized_) {
      errorMessage = "Miniaudio device is unavailable";
      return false;
    }
    const auto observed = observeState();
    if (observed.state == audio::playback::BackendRunState::Running) {
      return true;
    }
    if (observed.state == audio::playback::BackendRunState::Unknown) {
      errorMessage = observed.diagnostic;
      return false;
    }
    const ma_result result = ma_device_start(&device_);
    if (result != MA_SUCCESS) {
      errorMessage = std::string("Miniaudio start failed: ") +
                     ma_result_description(result);
      return false;
    }
    return true;
  }

  bool stop(std::string &errorMessage) override {
    if (!initialized_) {
      return true;
    }
    const auto observed = observeState();
    if (observed.state == audio::playback::BackendRunState::Stopped) {
      return true;
    }
    if (observed.state == audio::playback::BackendRunState::Unknown) {
      errorMessage = observed.diagnostic;
      return false;
    }
    const ma_result result = ma_device_stop(&device_);
    if (result != MA_SUCCESS) {
      errorMessage = std::string("Miniaudio stop failed: ") +
                     ma_result_description(result);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool isStarted() const override {
    return observeState().state == audio::playback::BackendRunState::Running;
  }

  [[nodiscard]] audio::playback::BackendStateObservation
  observeState() const override {
    if (!initialized_) {
      return {.state = audio::playback::BackendRunState::Unknown,
              .diagnostic = "Miniaudio device is unavailable"};
    }
    switch (ma_device_get_state(&device_)) {
    case ma_device_state_stopped:
      return {.state = audio::playback::BackendRunState::Stopped};
    case ma_device_state_started:
      return {.state = audio::playback::BackendRunState::Running};
    default:
      return {.state = audio::playback::BackendRunState::Unknown,
              .diagnostic = "Miniaudio device is transitioning or unavailable"};
    }
  }

  [[nodiscard]] RuntimeState runtimeState() const override { return state_; }

private:
  static void dataCallback(ma_device *device, void *output, const void *,
                           ma_uint32 frameCount) {
    auto *self = static_cast<MiniaudioStream *>(device->pUserData);
    if (self == nullptr || self->renderCallback_ == nullptr) {
      fillSilence(output, frameCount,
                  static_cast<int>(device->playback.channels));
      return;
    }
    self->renderCallback_(output, frameCount,
                          static_cast<int>(device->playback.channels),
                          self->renderUserData_);
  }

  StreamRequest request_;
  RenderCallback renderCallback_ = nullptr;
  void *renderUserData_ = nullptr;
  ma_device device_{};
  RuntimeState state_;
  bool initialized_ = false;
};

class MiniaudioFactory final : public IBackendFactory {
public:
  [[nodiscard]] Capabilities capabilities() const override {
    return {.outputDevices = {{.name = "System Output", .isDefault = true}}};
  }

  std::unique_ptr<IBackend> open(const StreamRequest &request,
                                 RenderCallback renderCallback,
                                 void *renderUserData,
                                 std::string &errorMessage) override {
    errorMessage.clear();
    if (!request.deviceId.empty() || request.sampleRate != 0 ||
        request.bufferFrames != 0) {
      errorMessage =
          "This platform uses the system-managed audio device and latency";
      return nullptr;
    }
    auto stream = std::make_unique<MiniaudioStream>(
        request, renderCallback, renderUserData, errorMessage);
    return stream->valid() ? std::move(stream) : nullptr;
  }
};

#if TARGET_OS_DESKTOP || TARGET_OS_LINUX

std::uint64_t fnv1a64(std::string_view value) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string hex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(16, '0');
  for (std::size_t index = result.size(); index > 0; --index) {
    result[index - 1] = digits[value & 0xFU];
    value >>= 4U;
  }
  return result;
}

struct PortAudioDeviceRecord {
  DeviceInfo info;
  PaDeviceIndex index = paNoDevice;
  double defaultSampleRate = 0.0;
  double defaultLowOutputLatency = 0.0;
};

std::vector<PortAudioDeviceRecord> enumeratePortAudioDevices() {
  constexpr std::array<std::uint32_t, 6> sampleRates{44100, 48000,  88200,
                                                     96000, 176400, 192000};
  constexpr std::array<std::uint32_t, 7> bufferFrames{0,   64,   128, 256,
                                                      512, 1024, 2048};

  std::vector<PortAudioDeviceRecord> result;
  const int deviceCount = Pa_GetDeviceCount();
  if (deviceCount < 0) {
    return result;
  }
  const PaDeviceIndex defaultOutput = Pa_GetDefaultOutputDevice();
  std::map<std::string, std::size_t> duplicateOrdinals;
  for (PaDeviceIndex index = 0; index < deviceCount; ++index) {
    const PaDeviceInfo *device = Pa_GetDeviceInfo(index);
    if (device == nullptr || device->maxOutputChannels < kOutputChannels) {
      continue;
    }
    const PaHostApiInfo *host = Pa_GetHostApiInfo(device->hostApi);
    const std::string hostName =
        host != nullptr && host->name != nullptr ? host->name : "unknown";
    const std::string deviceName =
        device->name != nullptr ? device->name : "Unnamed Output";
    const std::string fingerprint = hostName + "\n" + deviceName;
    const std::size_t ordinal = ++duplicateOrdinals[fingerprint];

    PaStreamParameters parameters{};
    parameters.device = index;
    parameters.channelCount = kOutputChannels;
    parameters.sampleFormat = paInt16;
    parameters.suggestedLatency = device->defaultLowOutputLatency;

    PortAudioDeviceRecord record;
    record.info.id = "portaudio:" + hex64(fnv1a64(fingerprint)) + ":" +
                     std::to_string(ordinal);
    record.info.name = hostName + " — " + deviceName;
    record.info.isDefault = index == defaultOutput;
    for (const auto rate : sampleRates) {
      if (Pa_IsFormatSupported(nullptr, &parameters,
                               static_cast<double>(rate)) ==
          paFormatIsSupported) {
        record.info.sampleRates.push_back(rate);
      }
    }
    record.index = index;
    record.defaultSampleRate = device->defaultSampleRate;
    record.defaultLowOutputLatency = device->defaultLowOutputLatency;
    std::optional<NativeBufferFrameLimits> nativeBufferLimits;
#if TARGET_OS_WINDOWS
    if (host != nullptr && host->type == paASIO) {
      long minimum = 0;
      long maximum = 0;
      long preferred = 0;
      long granularity = 0;
      if (PaAsio_GetAvailableBufferSizes(index, &minimum, &maximum, &preferred,
                                         &granularity) == paNoError &&
          minimum > 0 && maximum >= minimum && preferred > 0 &&
          maximum <= std::numeric_limits<std::uint32_t>::max() &&
          preferred <= std::numeric_limits<std::uint32_t>::max() &&
          granularity >= -1 &&
          granularity <= std::numeric_limits<std::int32_t>::max()) {
        nativeBufferLimits = {
            .minimum = static_cast<std::uint32_t>(minimum),
            .maximum = static_cast<std::uint32_t>(maximum),
            .preferred = static_cast<std::uint32_t>(preferred),
            .granularity = static_cast<std::int32_t>(granularity),
        };
      }
    }
#endif
    record.info.bufferFrames =
        SelectPortAudioBufferFrameOptions(bufferFrames, nativeBufferLimits);
    result.push_back(std::move(record));
  }
  return result;
}

class PortAudioStream final : public IBackend {
public:
  PortAudioStream(const PortAudioDeviceRecord &device,
                  const StreamRequest &request, RenderCallback renderCallback,
                  void *renderUserData, std::string &errorMessage)
      : renderCallback_(renderCallback), renderUserData_(renderUserData) {
    PaStreamParameters parameters{};
    parameters.device = device.index;
    parameters.channelCount = kOutputChannels;
    parameters.sampleFormat = paInt16;
    parameters.suggestedLatency = device.defaultLowOutputLatency;
    const double sampleRate = request.sampleRate == 0
                                  ? device.defaultSampleRate
                                  : static_cast<double>(request.sampleRate);
    const unsigned long frames = request.bufferFrames == 0
                                     ? paFramesPerBufferUnspecified
                                     : request.bufferFrames;
    const PaError result =
        Pa_OpenStream(&stream_, nullptr, &parameters, sampleRate, frames,
                      paNoFlag, &PortAudioStream::callback, this);
    if (result != paNoError) {
      errorMessage =
          std::string("PortAudio open failed: ") + Pa_GetErrorText(result);
      stream_ = nullptr;
      return;
    }
    state_.request = request;
    state_.effectiveSampleRate = static_cast<std::uint32_t>(sampleRate + 0.5);
    state_.effectiveBufferFrames = request.bufferFrames;
    if (const PaStreamInfo *info = Pa_GetStreamInfo(stream_); info != nullptr) {
      state_.effectiveSampleRate =
          static_cast<std::uint32_t>(info->sampleRate + 0.5);
      state_.effectiveLatencyMs = info->outputLatency * 1000.0;
      if (state_.effectiveBufferFrames == 0 && info->outputLatency > 0.0) {
        const double latencyFrames = info->outputLatency * info->sampleRate;
        if (latencyFrames <=
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
          state_.effectiveBufferFrames =
              static_cast<std::uint32_t>(latencyFrames + 0.5);
        }
      }
    }
  }

  ~PortAudioStream() override {
    if (stream_ != nullptr) {
      if (observeState().state != audio::playback::BackendRunState::Stopped) {
        (void)Pa_AbortStream(stream_);
      }
      (void)Pa_CloseStream(stream_);
    }
  }

  [[nodiscard]] bool valid() const { return stream_ != nullptr; }

  bool start(std::string &errorMessage) override {
    if (stream_ == nullptr) {
      errorMessage = "PortAudio stream is unavailable";
      return false;
    }
    const auto observed = observeState();
    if (observed.state == audio::playback::BackendRunState::Running) {
      return true;
    }
    if (observed.state == audio::playback::BackendRunState::Unknown) {
      errorMessage = observed.diagnostic;
      return false;
    }
    const PaError result = Pa_StartStream(stream_);
    if (result != paNoError) {
      errorMessage =
          std::string("PortAudio start failed: ") + Pa_GetErrorText(result);
      return false;
    }
    return true;
  }

  bool stop(std::string &errorMessage) override {
    if (stream_ == nullptr) {
      return true;
    }
    const auto observed = observeState();
    if (observed.state == audio::playback::BackendRunState::Stopped) {
      return true;
    }
    if (observed.state == audio::playback::BackendRunState::Unknown) {
      errorMessage = observed.diagnostic;
      return false;
    }
    const PaError result = Pa_StopStream(stream_);
    if (result != paNoError) {
      errorMessage =
          std::string("PortAudio stop failed: ") + Pa_GetErrorText(result);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool isStarted() const override {
    return observeState().state == audio::playback::BackendRunState::Running;
  }

  [[nodiscard]] audio::playback::BackendStateObservation
  observeState() const override {
    if (stream_ == nullptr) {
      return {.state = audio::playback::BackendRunState::Unknown,
              .diagnostic = "PortAudio stream is unavailable"};
    }
    const PaError state = Pa_IsStreamStopped(stream_);
    return audio::playback::InterpretStoppedQueryResult(
        state, state < 0 ? std::string("PortAudio state query failed: ") +
                               Pa_GetErrorText(state)
                         : std::string{});
  }

  [[nodiscard]] RuntimeState runtimeState() const override { return state_; }

private:
  static int callback(const void *, void *output, unsigned long frameCount,
                      const PaStreamCallbackTimeInfo *, PaStreamCallbackFlags,
                      void *userData) {
    auto *self = static_cast<PortAudioStream *>(userData);
    if (self == nullptr || self->renderCallback_ == nullptr ||
        frameCount > std::numeric_limits<std::uint32_t>::max()) {
      fillSilence(output,
                  static_cast<std::uint32_t>(std::min<unsigned long>(
                      frameCount, std::numeric_limits<std::uint32_t>::max())),
                  kOutputChannels);
      return paContinue;
    }
    self->renderCallback_(output, static_cast<std::uint32_t>(frameCount),
                          kOutputChannels, self->renderUserData_);
    return paContinue;
  }

  RenderCallback renderCallback_ = nullptr;
  void *renderUserData_ = nullptr;
  PaStream *stream_ = nullptr;
  RuntimeState state_;
};

class PortAudioFactory final : public IBackendFactory {
public:
  PortAudioFactory() {
    const PaError result = Pa_Initialize();
    if (result != paNoError) {
      throw std::runtime_error(std::string("PortAudio init failed: ") +
                               Pa_GetErrorText(result));
    }
    devices_ = enumeratePortAudioDevices();
  }

  ~PortAudioFactory() override { (void)Pa_Terminate(); }

  [[nodiscard]] Capabilities capabilities() const override {
    Capabilities result{.canSelectOutputDevice = true,
                        .canSelectSampleRate = true,
                        .canSelectBufferFrames = true};
    for (const auto &record : devices_) {
      result.outputDevices.push_back(record.info);
    }
    return result;
  }

  std::unique_ptr<IBackend> open(const StreamRequest &request,
                                 RenderCallback renderCallback,
                                 void *renderUserData,
                                 std::string &errorMessage) override {
    errorMessage.clear();
    const PortAudioDeviceRecord *selected = nullptr;
    if (request.deviceId.empty()) {
      const auto defaultDevice = std::find_if(
          devices_.begin(), devices_.end(),
          [](const auto &device) { return device.info.isDefault; });
      if (defaultDevice != devices_.end()) {
        selected = &*defaultDevice;
      } else if (!devices_.empty()) {
        selected = &devices_.front();
      }
    } else {
      const auto exact = std::find_if(
          devices_.begin(), devices_.end(), [&](const auto &device) {
            return device.info.id == request.deviceId;
          });
      if (exact != devices_.end()) {
        selected = &*exact;
      }
    }
    if (selected == nullptr) {
      errorMessage = request.deviceId.empty()
                         ? "No PortAudio output device is available"
                         : "Requested PortAudio output device is unavailable";
      return nullptr;
    }
    if (request.sampleRate != 0 &&
        !std::ranges::contains(selected->info.sampleRates,
                               request.sampleRate)) {
      errorMessage = "Requested PortAudio sample rate is unsupported";
      return nullptr;
    }
    if (request.bufferFrames != 0 &&
        !std::ranges::contains(selected->info.bufferFrames,
                               request.bufferFrames)) {
      errorMessage = "Requested PortAudio buffer size is unsupported";
      return nullptr;
    }
    auto stream = std::make_unique<PortAudioStream>(
        *selected, request, renderCallback, renderUserData, errorMessage);
    return stream->valid() ? std::move(stream) : nullptr;
  }

private:
  std::vector<PortAudioDeviceRecord> devices_;
};

class DesktopAudioFactory final : public IBackendFactory {
public:
  DesktopAudioFactory() {
    try {
      portAudio_ = std::make_unique<PortAudioFactory>();
    } catch (const std::exception &) {
      portAudio_.reset();
    }
  }

  [[nodiscard]] Capabilities capabilities() const override {
    if (portAudio_ != nullptr) {
      auto result = portAudio_->capabilities();
      if (!result.outputDevices.empty()) {
        return result;
      }
    }
    return fallback_.capabilities();
  }

  std::unique_ptr<IBackend> open(const StreamRequest &request,
                                 RenderCallback renderCallback,
                                 void *renderUserData,
                                 std::string &errorMessage) override {
    if (portAudio_ != nullptr) {
      auto stream = portAudio_->open(request, renderCallback, renderUserData,
                                     errorMessage);
      if (stream != nullptr) {
        return stream;
      }
    }
    const bool isDefaultRequest = request.deviceId.empty() &&
                                  request.sampleRate == 0 &&
                                  request.bufferFrames == 0;
    if (!isDefaultRequest) {
      if (errorMessage.empty()) {
        errorMessage = "Requested PortAudio configuration is unavailable";
      }
      return nullptr;
    }
    std::string fallbackError;
    auto fallback =
        fallback_.open(request, renderCallback, renderUserData, fallbackError);
    if (fallback != nullptr) {
      errorMessage.clear();
      return fallback;
    }
    if (errorMessage.empty()) {
      errorMessage = std::move(fallbackError);
    }
    return nullptr;
  }

private:
  std::unique_ptr<PortAudioFactory> portAudio_;
  MiniaudioFactory fallback_;
};

#endif

} // namespace

std::unique_ptr<IBackendFactory> CreatePlatformBackendFactory() {
#if TARGET_OS_DESKTOP || TARGET_OS_LINUX
  return std::make_unique<DesktopAudioFactory>();
#else
  return std::make_unique<MiniaudioFactory>();
#endif
}

} // namespace audio
