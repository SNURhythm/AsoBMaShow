#pragma once

#include <memory>

namespace bms_parser { class Chart; }

namespace settings_scene {
inline constexpr long long kPreviewLoopMicros = 8000000LL;
inline constexpr double kPreviewBpm = 120.0;

[[nodiscard]] std::unique_ptr<bms_parser::Chart> makePreviewChart();
} // namespace settings_scene
