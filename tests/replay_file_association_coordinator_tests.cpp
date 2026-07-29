#include "replay/ReplayFileAssociationCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace replay;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

struct Harness {
  std::string attemptId = "123e4567-e89b-42d3-a456-426614174000";
  std::string stem = repeated('a', 64);
  std::int64_t playedAt = 1'700'000'000'123LL;
  std::vector<std::string> events;
  ModernReplayReservationStatus reservationStatus =
      ModernReplayReservationStatus::Reserved;
  ModernReplayReservationReleaseOutcome released{
      .status = ModernReplayReservationReleaseStatus::Released};
  bool encodeSucceeds = true;
  bool fileReservationSucceeds = true;
  bool reservationOwnsInstalledFile = false;
  ModernReplayOwnershipRecordStatus ownershipStatus =
      ModernReplayOwnershipRecordStatus::Recorded;
  ReplayInstallOutcome installed;
  ReplayFileInspection inspected{.state = ReplayFileState::Missing};
  bool cleanupSucceeds = true;
  std::int64_t nextHistory = 0;

  Harness() {
    std::string diagnostic;
    const auto identity = pathForStem(stem, 0, diagnostic);
    const ReplayFileMetadata file{
        .relativePath = identity->relativePath,
        .sha256 = repeated('b', 64),
        .compressedSize = 1,
        .codecVersion = BeatorajaReplayCodec::kCodecVersion,
    };
    installed = {
        .state = ReplayInstallState::InstalledVerified,
        .file = ReplayInstalledFile{
            .metadata = file,
            .attemptToken = attemptId,
            .lifecycle =
                {.state = ReplayFileLifecycleState::InstalledUnassociated,
                 .attemptToken = attemptId,
                 .receipt = ReplayFileOwnershipReceipt{
                     .attemptToken = attemptId, .metadata = file}}},
    };
  }

  ReplayFileAssociationCoordinatorDependencies dependencies() {
    return {
        .reservePath =
            [this](std::string_view requestedAttempt,
                   std::string_view requestedStem, std::int64_t requestedAt) {
              events.emplace_back("reserve-path");
              if (reservationStatus != ModernReplayReservationStatus::Reserved &&
                  reservationStatus !=
                      ModernReplayReservationStatus::AlreadyReserved) {
                return ModernReplayReservationOutcome{
                    .status = reservationStatus,
                    .diagnostic = "path reservation failed"};
              }
              std::string diagnostic;
              const auto identity =
                  pathForStem(requestedStem, nextHistory++, diagnostic);
              std::optional<ReplayFileMetadata> ownedFile;
              if (reservationOwnsInstalledFile) {
                ownedFile = ReplayFileMetadata{
                    .relativePath = identity->relativePath,
                    .sha256 = repeated('b', 64),
                    .compressedSize = 1,
                    .codecVersion = BeatorajaReplayCodec::kCodecVersion};
              }
              return ModernReplayReservationOutcome{
                  .status = reservationStatus,
                  .reservation = ModernReplayPathReservation{
                      .attemptId = std::string(requestedAttempt),
                      .identity = *identity,
                      .createdAtUnixMillis = requestedAt,
                      .ownedFile = std::move(ownedFile)}};
            },
        .releasePath =
            [this](const ModernReplayPathReservation &) {
              events.emplace_back("release-path");
              return released;
            },
        .reserveFile =
            [this](const ReplayPathIdentity &identity,
                   std::span<const std::byte>, std::string_view token) {
              events.emplace_back("reserve-file");
              if (!fileReservationSucceeds) {
                return ReplayReservationOutcome{
                    .diagnostic = "file reservation failed"};
              }
              return ReplayReservationOutcome{
                  .reservation = ReplayFileReservation{
                      .identity = identity,
                      .attemptToken = std::string(token),
                      .expectedMetadata =
                          {.relativePath = identity.relativePath,
                           .sha256 = repeated('b', 64),
                           .compressedSize = 1,
                           .codecVersion =
                               BeatorajaReplayCodec::kCodecVersion},
                      .temporaryRelativePath = "replay/private.tmp"}};
            },
        .installFile =
            [this](const ReplayFileReservation &reservation,
                   std::span<const std::byte>,
                   const ReplayInstallOwnershipJournal &journal) {
              if (!installed.existingIdenticalFile) {
                std::string diagnostic;
                const ReplayFileOwnershipReceipt receipt{
                    .attemptToken = reservation.attemptToken,
                    .metadata = reservation.expectedMetadata};
                if (!journal || !journal(receipt, diagnostic)) {
                  return ReplayInstallOutcome{
                      .state = ReplayInstallState::Failed,
                      .diagnostic = std::move(diagnostic)};
                }
              }
              events.emplace_back("install");
              auto outcome = installed;
              if (outcome.file) {
                outcome.file->metadata = reservation.expectedMetadata;
                if (outcome.file->lifecycle.receipt) {
                  outcome.file->lifecycle.receipt->metadata =
                      reservation.expectedMetadata;
                }
              }
              return outcome;
            },
        .recordInstallIntent =
            [this](const ModernReplayPathReservation &reservation,
                   const ReplayFileOwnershipReceipt &receipt) {
              events.emplace_back("record-ownership");
              if (ownershipStatus !=
                      ModernReplayOwnershipRecordStatus::Recorded &&
                  ownershipStatus !=
                      ModernReplayOwnershipRecordStatus::AlreadyRecorded) {
                return ModernReplayOwnershipRecordOutcome{
                    .status = ownershipStatus,
                    .diagnostic = "ownership recording failed"};
              }
              auto owned = reservation;
              owned.ownedFile = receipt.metadata;
              return ModernReplayOwnershipRecordOutcome{
                  .status = ownershipStatus,
                  .reservation = std::move(owned)};
            },
        .inspectFile =
            [this](const ReplayFileMetadata &) {
              events.emplace_back("inspect");
              return inspected;
            },
        .removeIfMatches =
            [this](const ReplayFileMetadata &, std::string &diagnostic) {
              events.emplace_back("cleanup");
              if (!cleanupSucceeds) {
                diagnostic = "cleanup failed";
              }
              return cleanupSucceeds;
            },
    };
  }

  ReplayFileEncoder encoder() {
    return [this](std::string &diagnostic)
               -> std::optional<std::vector<std::byte>> {
      events.emplace_back("encode");
      if (!encodeSucceeds) {
        diagnostic = "encode failed";
        return std::nullopt;
      }
      return std::vector<std::byte>{std::byte{0x42}};
    };
  }
};

void testSuccessEncodeFailureAndIntegrityConflict() {
  Harness success;
  ReplayFileAssociationCoordinator coordinator(success.dependencies());
  const auto associated = coordinator.associate(
      success.attemptId, success.stem, success.playedAt, success.encoder());
  expect(associated.status == ReplayFileAssociationStatus::Attached &&
             associated.association &&
             associated.association->ownership ==
                 ReplayFileInstalledOwnership::CreatedByAttempt &&
             success.events ==
                 std::vector<std::string>({"reserve-path", "encode",
                                           "reserve-file", "record-ownership",
                                           "install"}),
         "successful association journals ownership before installing bytes");

  Harness encodeFailure;
  encodeFailure.encodeSucceeds = false;
  ReplayFileAssociationCoordinator failed(encodeFailure.dependencies());
  const auto omitted = failed.associate(
      encodeFailure.attemptId, encodeFailure.stem, encodeFailure.playedAt,
      encodeFailure.encoder());
  expect(omitted.status == ReplayFileAssociationStatus::Omitted &&
             !omitted.association &&
             encodeFailure.events ==
                 std::vector<std::string>(
                     {"reserve-path", "encode", "release-path"}),
         "encode failure releases the exact database reservation");

  Harness conflict;
  conflict.reservationStatus = ModernReplayReservationStatus::IntegrityConflict;
  ReplayFileAssociationCoordinator rejected(conflict.dependencies());
  const auto conflicted = rejected.associate(
      conflict.attemptId, conflict.stem, conflict.playedAt, conflict.encoder());
  expect(conflicted.status == ReplayFileAssociationStatus::IntegrityConflict &&
             conflict.events == std::vector<std::string>({"reserve-path"}),
         "reservation identity conflict stops before encoding or file writes");
}

void testOwnershipJournalFailureStopsBeforeInstallingBytes() {
  Harness failed;
  failed.ownershipStatus = ModernReplayOwnershipRecordStatus::StorageFailure;
  ReplayFileAssociationCoordinator coordinator(failed.dependencies());
  const auto omitted = coordinator.associate(
      failed.attemptId, failed.stem, failed.playedAt, failed.encoder());
  expect(omitted.status == ReplayFileAssociationStatus::Omitted &&
             !omitted.association &&
             failed.events ==
                 std::vector<std::string>({"reserve-path", "encode",
                                           "reserve-file", "record-ownership",
                                           "inspect", "release-path"}),
         "ownership journal failure stops before final installation");
}

void testOwnedRetryEncodeFailureRetainsItsReservation() {
  Harness retry;
  retry.reservationStatus = ModernReplayReservationStatus::AlreadyReserved;
  retry.reservationOwnsInstalledFile = true;
  retry.encodeSucceeds = false;
  ReplayFileAssociationCoordinator coordinator(retry.dependencies());

  const auto omitted = coordinator.associate(
      retry.attemptId, retry.stem, retry.playedAt, retry.encoder());
  expect(omitted.status == ReplayFileAssociationStatus::Omitted &&
             !omitted.association &&
             retry.events ==
                 std::vector<std::string>({"reserve-path", "encode"}),
         "failed retry encoding retains the reservation that owns the only "
         "installed BRD receipt");
}

void testOccupiedAndAmbiguousInstallPaths() {
  Harness occupied;
  int installs = 0;
  auto dependencies = occupied.dependencies();
  const auto baseInstall = dependencies.installFile;
  dependencies.installFile =
      [&, baseInstall](const ReplayFileReservation &reservation,
                       std::span<const std::byte> bytes,
                       const ReplayInstallOwnershipJournal &journal) {
        if (installs++ == 0) {
          occupied.events.emplace_back("install");
          return ReplayInstallOutcome{.state = ReplayInstallState::Occupied,
                                      .diagnostic = "occupied"};
        }
        return baseInstall(reservation, bytes, journal);
      };
  ReplayFileAssociationCoordinator coordinator(std::move(dependencies));
  const auto associated = coordinator.associate(
      occupied.attemptId, occupied.stem, occupied.playedAt, occupied.encoder());
  expect(associated.status == ReplayFileAssociationStatus::Attached &&
             associated.association &&
             associated.association->attachment.identity.historyIndex == 1 &&
             std::ranges::count(occupied.events, "reserve-path") == 2 &&
             std::ranges::count(occupied.events, "release-path") == 1 &&
             std::ranges::count(occupied.events, "encode") == 1,
         "occupied destination advances the shared history slot without "
         "re-encoding");

  Harness ambiguous;
  ambiguous.installed = {
      .state = ReplayInstallState::RetryableAmbiguous,
      .diagnostic = "install acknowledgement lost"};
  ambiguous.inspected = {.state = ReplayFileState::Available};
  ReplayFileAssociationCoordinator reconcile(ambiguous.dependencies());
  const auto recovered = reconcile.associate(
      ambiguous.attemptId, ambiguous.stem, ambiguous.playedAt,
      ambiguous.encoder());
  expect(recovered.status == ReplayFileAssociationStatus::Attached &&
             recovered.association &&
             recovered.association->ownership ==
                 ReplayFileInstalledOwnership::Ambiguous &&
             std::ranges::find(ambiguous.events, "inspect") !=
                 ambiguous.events.end(),
         "verified installed bytes survive an ambiguous acknowledgement");
}

void testDefinitiveCleanupUsesOwnershipAndExactMetadata() {
  Harness created;
  ReplayFileAssociationCoordinator coordinator(created.dependencies());
  const auto associated = coordinator.associate(
      created.attemptId, created.stem, created.playedAt, created.encoder());
  std::string diagnostic;
  expect(associated.association &&
             coordinator.abandonDefinitively(*associated.association,
                                             diagnostic) &&
             std::ranges::find(created.events, "cleanup") !=
                 created.events.end() &&
             std::ranges::find(created.events, "release-path") !=
                 created.events.end(),
         "definitive rejection removes exact attempt-created bytes then "
         "releases ownership");

  Harness preexisting;
  preexisting.installed.existingIdenticalFile = true;
  ReplayFileAssociationCoordinator existing(preexisting.dependencies());
  const auto reused = existing.associate(preexisting.attemptId,
                                         preexisting.stem,
                                         preexisting.playedAt,
                                         preexisting.encoder());
  diagnostic.clear();
  expect(reused.association &&
             existing.abandonDefinitively(*reused.association, diagnostic) &&
             std::ranges::find(preexisting.events, "record-ownership") ==
                 preexisting.events.end() &&
             std::ranges::find(preexisting.events, "cleanup") ==
                 preexisting.events.end() &&
             std::ranges::find(preexisting.events, "release-path") !=
                 preexisting.events.end(),
         "pre-existing identical bytes are never deleted by another attempt");

  Harness ownedRetry;
  ownedRetry.reservationStatus =
      ModernReplayReservationStatus::AlreadyReserved;
  ownedRetry.reservationOwnsInstalledFile = true;
  ownedRetry.installed.existingIdenticalFile = true;
  ReplayFileAssociationCoordinator retry(ownedRetry.dependencies());
  const auto retried = retry.associate(ownedRetry.attemptId, ownedRetry.stem,
                                       ownedRetry.playedAt,
                                       ownedRetry.encoder());
  diagnostic.clear();
  expect(retried.association &&
             retried.association->ownership ==
                 ReplayFileInstalledOwnership::CreatedByAttempt &&
             retry.abandonDefinitively(*retried.association, diagnostic) &&
             std::ranges::find(ownedRetry.events, "cleanup") !=
                 ownedRetry.events.end() &&
             std::ranges::find(ownedRetry.events, "release-path") !=
                 ownedRetry.events.end(),
         "retry preserves durable attempt ownership of identical installed "
         "bytes");

  Harness ambiguous;
  ambiguous.installed = {.state = ReplayInstallState::RetryableAmbiguous};
  ambiguous.inspected = {.state = ReplayFileState::Available};
  ReplayFileAssociationCoordinator uncertain(ambiguous.dependencies());
  const auto retained = uncertain.associate(
      ambiguous.attemptId, ambiguous.stem, ambiguous.playedAt,
      ambiguous.encoder());
  diagnostic.clear();
  expect(retained.association &&
             !uncertain.abandonDefinitively(*retained.association,
                                            diagnostic) &&
             std::ranges::find(ambiguous.events, "cleanup") ==
                 ambiguous.events.end() &&
             std::ranges::find(ambiguous.events, "release-path") ==
                 ambiguous.events.end(),
         "ambiguous installed ownership remains untouched for reconciliation");
}

} // namespace

int main() {
  testSuccessEncodeFailureAndIntegrityConflict();
  testOwnershipJournalFailureStopsBeforeInstallingBytes();
  testOwnedRetryEncodeFailureRetainsItsReservation();
  testOccupiedAndAmbiguousInstallPaths();
  testDefinitiveCleanupUsesOwnershipAndExactMetadata();
  if (failures != 0) {
    std::cerr << failures << " replay association coordinator test(s) failed\n";
    return 1;
  }
  std::cout << "replay association coordinator tests passed\n";
  return 0;
}
