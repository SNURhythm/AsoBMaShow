#pragma once

#include "../repositories/ReplayRepository.h"

namespace replay {

enum class ModernReplayReservationCommitState {
  Unassociated,
  ResultOnly,
  ReplayAttached,
};

struct ModernReplayReservationReconciliationEntry {
  ModernReplayPathReservation reservation;
  ModernReplayReservationCommitState commitState =
      ModernReplayReservationCommitState::Unassociated;

  bool operator==(
      const ModernReplayReservationReconciliationEntry &) const = default;
};

struct ModernReplayReservationReconciliationOutcome {
  ModernReplayFileInventoryStatus status =
      ModernReplayFileInventoryStatus::StorageFailure;
  std::vector<ModernReplayReservationReconciliationEntry> entries;
  std::string diagnostic;
};

// Loads complete replay ownership only after every reference agrees with its
// exact saved result. Profile transfer consumes this strict snapshot.
[[nodiscard]] ModernReplayFileInventoryOutcome
loadAgreedModernReplayFileInventory(ReplayRepository &repository) noexcept;

// Loads only deletion tombstones for startup cleanup while applying the same
// exact result/reference agreement as complete profile inventories.
[[nodiscard]] ModernReplayFileInventoryOutcome
loadAgreedModernReplayTombstoneInventory(
    ReplayRepository &repository) noexcept;

// Classifies only durable path reservations, consulting both result owners so
// restart cleanup never removes a final file whose attachment committed even
// if the original staging acknowledgement was lost.
[[nodiscard]] ModernReplayReservationReconciliationOutcome
loadAgreedModernReplayPathReservationInventory(
    ReplayRepository &repository) noexcept;

} // namespace replay
