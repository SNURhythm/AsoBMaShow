#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace video {
struct MetalDisplayLinkState;

// One instance on the application/Metal owner thread. UIKit and audio never
// wait on this object's run loop. Native API availability is checked at runtime.
class MetalDisplayLink {
public:
  // Notify the owner when the Metal link takes or releases the refresh clock.
  explicit MetalDisplayLink(std::function<void(bool)> attachmentChanged = {});
  ~MetalDisplayLink();
  MetalDisplayLink(const MetalDisplayLink &) = delete;
  MetalDisplayLink &operator=(const MetalDisplayLink &) = delete;

  bool beginFrame(void *layer, float refreshRate, std::uint32_t frameCap,
                  std::uint64_t generation, std::function<bool()> isCurrent);
  bool hasFrame() const;
  bool didSchedulePresent() const;
  void endFrame();
  void pause();
  static void pauseCurrent();

private:
  std::unique_ptr<MetalDisplayLinkState> state_;
};
} // namespace video
