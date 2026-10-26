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
  std::uint64_t voiceId = 0;
  bool setVoiceGain = false;
  float gain = 1.0F;
};

[[nodiscard]] bool replayEventAllowedForPlayback(
    const ReplayEvent &event,
    std::optional<gameplay::GameplayTimeRange> allowedRange,
    std::optional<gameplay::GameplayTimeRange> preparationRange =
        std::nullopt) noexcept;

[[nodiscard]] std::vector<ReplayKeysoundEvent> resolveReplayKeysounds(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events,
    std::optional<gameplay::GameplayTimeRange> allowedRange,
    std::optional<gameplay::GameplayTimeRange> preparationRange =
        std::nullopt,
    GameplayRuleset ruleset = GameplayRuleset::Beatoraja,
    std::span<const ReplayAudioUpdate> audioUpdates = {});

[[nodiscard]] std::vector<ScheduledAudioEvent> buildReplayKeysoundSchedule(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events, long long audioOffsetMicros,
    std::optional<gameplay::GameplayTimeRange> allowedRange,
    std::optional<gameplay::GameplayTimeRange> preparationRange =
        std::nullopt,
    GameplayRuleset ruleset = GameplayRuleset::Beatoraja,
    std::span<const ReplayAudioUpdate> audioUpdates = {});
