#pragma once

#include "AudioMix.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace audio {

struct DeviceInfo {
  std::string id;
  std::string name;
  bool isDefault = false;
  std::vector<std::uint32_t> sampleRates;
  std::vector<std::uint32_t> bufferFrames;
  bool operator==(const DeviceInfo &) const = default;
};

struct Capabilities {
  bool canSelectOutputDevice = false;
  bool canSelectSampleRate = false;
  bool canSelectBufferFrames = false;
  std::vector<DeviceInfo> outputDevices;
  bool operator==(const Capabilities &) const = default;
};

struct StreamRequest {
  std::string deviceId;
  std::uint32_t sampleRate = 0;
  std::uint32_t bufferFrames = 0;
  bool operator==(const StreamRequest &) const = default;
};

struct RuntimeState {
  StreamRequest request;
  std::uint32_t effectiveSampleRate = 0;
  std::uint32_t effectiveBufferFrames = 0;
  double effectiveLatencyMs = 0.0;
  std::uint32_t effectiveCallbackSampleRate = 0;
  bool operator==(const RuntimeState &) const = default;
};

using RenderCallback = void (*)(void *, std::uint32_t, int, void *);
// Native backends consume signed 16-bit PCM. Software submixes retain float
// headroom so the output graph can sum them before clipping and conversion.
// Float32Additive adds to the supplied buffer and leaves it untouched for
// silence. This lets an idle secondary submix cost no per-sample work.
enum class RenderFormat { Signed16, Float32, Float32Additive };
// Timestamp of the first output frame, supplied by the native audio API.
// A missing timestamp is not replaced with a latency estimate.
struct RenderTiming {
  std::int64_t outputSteadyMicros = 0;
  bool outputTimestampKnown = false;
  // Software fallback origin for the first frame when splitting a callback
  // without a native timestamp. This is not a hardware presentation estimate.
  std::int64_t fallbackSteadyMicros = 0;
};
using RenderTimingCallback = void (*)(RenderTiming, void *);
struct NativeBufferFrameLimits {
  std::uint32_t minimum = 0;
  std::uint32_t maximum = 0;
  std::uint32_t preferred = 0;
  std::int32_t granularity = 0;
  bool operator==(const NativeBufferFrameLimits &) const = default;
};

std::vector<std::uint32_t>
SelectPortAudioBufferFrameOptions(
    std::span<const std::uint32_t> candidates,
    std::optional<NativeBufferFrameLimits> nativeLimits);

class IBackend {
public:
  virtual ~IBackend() = default;
  virtual void setRenderTimingCallback(RenderTimingCallback, void *) {}
  virtual bool start(std::string &errorMessage) = 0;
  virtual bool stop(std::string &errorMessage) = 0;
  [[nodiscard]] virtual bool isStarted() const = 0;
  [[nodiscard]] virtual audio::playback::BackendStateObservation
  observeState() const {
    return {.state = isStarted() ? audio::playback::BackendRunState::Running
                                 : audio::playback::BackendRunState::Stopped};
  }
  [[nodiscard]] virtual RuntimeState runtimeState() const = 0;
  [[nodiscard]] virtual std::optional<bool> outputIsBluetooth(bool = false) const { return std::nullopt; }
};

class IBackendFactory {
public:
  virtual ~IBackendFactory() = default;
  [[nodiscard]] virtual RenderFormat renderFormat() const { return RenderFormat::Signed16; }
  [[nodiscard]] virtual Capabilities capabilities() const = 0;
  virtual std::unique_ptr<IBackend> open(const StreamRequest &request,
                                         RenderCallback renderCallback,
                                         void *renderUserData,
                                         std::string &errorMessage) = 0;
};

std::unique_ptr<IBackendFactory> CreatePlatformBackendFactory();

} // namespace audio
