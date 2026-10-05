#pragma once

#include <memory>
#include <array>

namespace bms_parser { class Chart; }

namespace settings_scene {
inline constexpr long long kPreviewLoopMicros = 33'000'000LL;
inline constexpr double kPreviewBpm = 120.0;
inline constexpr std::array<int, 9> kPreviewKeyModes{4, -5, 5, 6, -7, 7, 8, 10, 14};

[[nodiscard]] std::unique_ptr<bms_parser::Chart> makePreviewChart(int keyMode = 7);
} // namespace settings_scene
