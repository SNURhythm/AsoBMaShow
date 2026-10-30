#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace video {
struct MetalDisplayLinkState;

// One instance on the application/Metal owner thread. UIKit and audio never
// wait on this object's run loop. Native API availability is checked at runtime.
class MetalDisplayLink {
public:
  // Owner-thread notification when the link takes/releases refresh ownership,
  // so a platform's fallback clock need not run alongside it. Not per frame.
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
  // Diagnostic branch only. Called on the application/Metal owner thread.
  static bool requestRecreationForDiagnostics();
  static std::string diagnosticsForCurrentThread();

private:
  std::unique_ptr<MetalDisplayLinkState> state_;
};
} // namespace video
