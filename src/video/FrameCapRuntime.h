#pragma once

#include <cstdint>
#include <string>

namespace display {
class IFrameCapRuntime {
public:
  virtual ~IFrameCapRuntime() = default;
  virtual std::uint32_t currentFrameCap() const = 0;
  virtual bool applyFrameCap(std::uint32_t frameCap,
                             std::string &errorMessage) = 0;
};
} // namespace display
