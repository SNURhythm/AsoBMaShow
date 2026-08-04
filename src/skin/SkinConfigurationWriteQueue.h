#pragma once

#include "SkinProfileSettings.h"
#include "beatoraja/PlaySkinStateBridge.h"
#include "package/SkinPackageTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace skin {

struct SkinConfigurationWriteRequest {
  std::uint64_t sessionSerial = 0;
  SkinProfileId profileId;
  SkinEntryId entry;
  std::string expectedRevisionDigest;
  std::string expectedConfigurationDigest;
  std::uint64_t frameSerial = 0;
  std::vector<PersistedSkinConfigurationWrite> orderedWrites;
};

enum class SkinConfigurationEnqueueResult : std::uint8_t {
  Enqueued,
  QueueFull,
  Closed,
};

class SkinConfigurationWriteQueue final {
public:
  static constexpr std::size_t maxPending = 256;

  SkinConfigurationEnqueueResult
  enqueue(SkinConfigurationWriteRequest request) noexcept;
  [[nodiscard]] std::vector<SkinConfigurationWriteRequest> drain();
  void close() noexcept;

private:
  std::mutex mutex_;
  std::array<std::optional<SkinConfigurationWriteRequest>, maxPending> pending_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  bool closed_ = false;
};

} // namespace skin
