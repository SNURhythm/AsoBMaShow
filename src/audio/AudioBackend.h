#pragma once

#include "AudioMix.h"

#include <cstdint>
#include <memory>
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
  bool operator==(const RuntimeState &) const = default;
};

using RenderCallback = void (*)(void *, std::uint32_t, int, void *);

class IBackend {
public:
  virtual ~IBackend() = default;
  virtual bool start(std::string &errorMessage) = 0;
  virtual bool stop(std::string &errorMessage) = 0;
  [[nodiscard]] virtual bool isStarted() const = 0;
  [[nodiscard]] virtual RuntimeState runtimeState() const = 0;
};

class IBackendFactory {
public:
  virtual ~IBackendFactory() = default;
  [[nodiscard]] virtual Capabilities capabilities() const = 0;
  virtual std::unique_ptr<IBackend> open(const StreamRequest &request,
                                         RenderCallback renderCallback,
                                         void *renderUserData,
                                         std::string &errorMessage) = 0;
};

std::unique_ptr<IBackendFactory> CreatePlatformBackendFactory();

} // namespace audio
