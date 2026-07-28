#pragma once

#include "ReplayFileStore.h"

#include "../repositories/ReplayRepository.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

enum class ReplayFileAssociationStatus {
  Attached,
  Omitted,
  IntegrityConflict,
};

enum class ReplayFileInstalledOwnership {
  CreatedByAttempt,
  PreexistingIdentical,
  Ambiguous,
};

struct ReplayFileAssociation {
  ModernReplayPathReservation reservation;
  ModernReplayFileAttachment attachment;
  ReplayFileInstalledOwnership ownership =
      ReplayFileInstalledOwnership::Ambiguous;

  bool operator==(const ReplayFileAssociation &) const = default;
};

struct ReplayFileAssociationOutcome {
  ReplayFileAssociationStatus status = ReplayFileAssociationStatus::Omitted;
  std::optional<ReplayFileAssociation> association;
  std::string diagnostic;
};

using ReplayFileEncoder = std::function<std::optional<std::vector<std::byte>>(
    std::string &diagnostic)>;

struct ReplayFileAssociationCoordinatorDependencies {
  std::function<ModernReplayReservationOutcome(std::string_view,
                                               std::string_view, std::int64_t)>
      reservePath;
  std::function<ModernReplayReservationReleaseOutcome(
      const ModernReplayPathReservation &)>
      releasePath;
  std::function<ReplayReservationOutcome(
      const ReplayPathIdentity &, std::span<const std::byte>, std::string_view)>
      reserveFile;
  std::function<ReplayInstallOutcome(
      const ReplayFileReservation &, std::span<const std::byte>,
      const ReplayInstallOwnershipJournal &)>
      installFile;
  std::function<ModernReplayOwnershipRecordOutcome(
      const ModernReplayPathReservation &,
      const ReplayFileOwnershipReceipt &)>
      recordInstallIntent;
  std::function<ReplayFileInspection(const ReplayFileMetadata &)> inspectFile;
  std::function<bool(const ReplayFileMetadata &, std::string &)>
      removeIfMatches;
};

class ReplayFileAssociationCoordinator {
public:
  explicit ReplayFileAssociationCoordinator(
      ReplayFileAssociationCoordinatorDependencies dependencies);

  [[nodiscard]] ReplayFileAssociationOutcome
  associate(std::string_view attemptId, std::string_view stem,
            std::int64_t playedAtUnixMillis,
            const ReplayFileEncoder &encode) const;

  // Called only after the result database has definitively rejected an
  // association. Ambiguous ownership is deliberately retained.
  bool abandonDefinitively(const ReplayFileAssociation &association,
                           std::string &diagnostic) const;

private:
  ReplayFileAssociationCoordinatorDependencies dependencies_;
};

} // namespace replay
