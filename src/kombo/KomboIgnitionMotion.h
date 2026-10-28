#pragma once
#include "../ui/Motion.h"
#include <optional>

namespace kombo {
inline constexpr std::uint64_t ignitionMillis = 900;
struct IgnitionFrame {
  float scale = 1, width = 1, height = 1, glow = 0, burst = 1;
  bool active = false;
};
// The scene owns the clock: view rebuilds and late resumes never restart ignition.
inline IgnitionFrame ignitionFrame(std::optional<std::uint64_t> startedAt,
                                  std::uint64_t now, bool reduced = false) {
  if (!startedAt || reduced) return {};
  const auto elapsed = now > *startedAt ? now - *startedAt : 0;
  if (elapsed >= ignitionMillis) return {};
  IgnitionFrame frame;
  frame.active = true;
  frame.burst = std::clamp((float(elapsed)-120) / 700, 0.0F, 1.0F);
  if (elapsed < 90) {
    const float t = ui::motion::ease(float(elapsed)/90, ui::motion::Ease::Smooth);
    frame.scale = .68F - .20F*t;
    frame.width = 1 + .15F*t;
    frame.height = 1 - .25F*t;
    frame.glow = .15F*t;
  } else if (elapsed < 300) {
    const float t = ui::motion::ease(float(elapsed-90)/210, ui::motion::Ease::Smooth);
    frame.scale = .48F + .65F*t;
    frame.width = 1.15F - .27F*t;
    frame.height = .75F + .35F*t;
    frame.glow = .15F + .85F*t;
  } else {
    const float t = ui::motion::ease(float(elapsed-300)/600, ui::motion::Ease::Smooth);
    frame.scale = 1.13F - .13F*t;
    frame.width = .88F + .12F*t;
    frame.height = 1.10F - .10F*t;
    frame.glow = 1-t;
  }
  return frame;
}
} // namespace kombo
