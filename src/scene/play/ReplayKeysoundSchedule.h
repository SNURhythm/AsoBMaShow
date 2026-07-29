#pragma once

#include "GameplaySimulation.h"
#include "../../ReplayData.h"
#include "../../audio/Jukebox.h"

#include <optional>
#include <span>
#include <vector>

struct ReplayKeysoundEvent {
  long long songTimeMicros = 0;
  int wav = bms_parser::Parser::NoWav;
};

[[nodiscard]] std::vector<ReplayKeysoundEvent> resolveReplayKeysounds(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events,
    std::optional<gameplay::GameplayTimeRange> allowedRange);

[[nodiscard]] std::vector<ScheduledAudioEvent> buildReplayKeysoundSchedule(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events, long long audioOffsetMicros,
    std::optional<gameplay::GameplayTimeRange> allowedRange);
