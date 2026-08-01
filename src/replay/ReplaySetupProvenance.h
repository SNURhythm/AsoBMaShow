#pragma once

#include "ReplaySetup.h"

#include "../ScoreProvenance.h"

#include <optional>
#include <string>

namespace replay {

[[nodiscard]] std::optional<ReplaySetup>
captureLocalReplaySetup(const LocalReplaySetupFacts &facts,
                        const ScoreProvenance &provenance,
                        std::string &diagnostic) noexcept;

[[nodiscard]] bool
replaySetupAgreesWithProvenance(const ReplaySetup &setup,
                                const ScoreProvenance &provenance) noexcept;

} // namespace replay
