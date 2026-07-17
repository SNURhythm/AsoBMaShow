#pragma once

#include "GameplaySimulation.h"
#include "../../ReplayData.h"
#include "../../audio/Jukebox.h"

#include <optional>
#include <span>
#include <vector>

[[nodiscard]] std::vector<ScheduledAudioEvent> buildReplayKeysoundSchedule(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events, long long audioOffsetMicros,
    std::optional<gameplay::GameplayTimeRange> allowedRange);
