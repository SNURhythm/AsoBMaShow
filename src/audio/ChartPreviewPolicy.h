#pragma once

namespace audio {
enum class ChartPreviewMode { Highlight = 0, Beginning = 1, PreviewFile = 2 };

constexpr bool ChartPreviewUsesHighlight(ChartPreviewMode mode, bool fallbackHighlight) {
  return mode == ChartPreviewMode::Highlight ||
         (mode == ChartPreviewMode::PreviewFile && fallbackHighlight);
}
} // namespace audio
