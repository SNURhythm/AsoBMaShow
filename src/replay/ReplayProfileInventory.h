#pragma once

#include "../repositories/ReplayRepository.h"

namespace replay {

// Loads replay ownership only after every reference agrees with its exact
// saved result. Cleanup and profile transfer consume this strict snapshot.
[[nodiscard]] ModernReplayFileInventoryOutcome
loadAgreedModernReplayFileInventory(ReplayRepository &repository) noexcept;

} // namespace replay
