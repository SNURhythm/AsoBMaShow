#include "replay/ReplayFileActionService.h"

#include "repositories/ReplayRepository.h"
#include "replay/BeatorajaReplayPath.h"
#include "replay/ReplayFileAssociationCoordinator.h"
#include "replay/ReplayFileReconciler.h"
#include "replay/ReplayFileStore.h"
#include "replay/ReplayProfileInventory.h"
#include "sqlite3.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-replay-actions-" + std::to_string(stamp));
    assert(std::filesystem::create_directories(path));
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

result_persistence::ModernChartResult result(int suffix, char hash) {
  result_persistence::ModernChartResult value;
  value.attemptId = "123e4567-e89b-42d3-a456-42661417400" +
                    std::to_string(suffix);
  value.score.chartPath = "library/chart.bms";
  value.score.chartMd5 = repeated(hash, 32);
  value.score.chartSha256 = repeated(hash, 64);
  value.score.chartTitle = "Title";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = 4;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = 82.5F;
  value.score.clearType = kClearTypeNormalClearRank;
  value.score.provenance = ScoreProvenance::Legacy();
  value.keyMode = 7;
  value.adoptedGaugeType = GaugeType::Normal;
  value.adoptedGaugeHistory = {20.0F, 82.5F};
  value.playedAtUnixMillis = 1'700'000'000'000LL + suffix;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  std::string diagnostic;
  assert(result_persistence::validateModernChartResult(value, diagnostic));
  return value;
}

struct InstalledResult {
  result_persistence::ModernChartResult result;
  ModernReplayFileReference reference;
};

InstalledResult installResult(ReplayRepository &repository,
                              replay::ReplayFileStore &store, int suffix,
                              char hash) {
  const auto completed = result(suffix, hash);
  const auto reserved = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(reserved.status == ModernReplayReservationStatus::Reserved &&
         reserved.reservation);
  const std::vector bytes{std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08},
                          std::byte{static_cast<unsigned char>(suffix)}};
  const auto fileReservation = store.reserve(
      reserved.reservation->identity, bytes, completed.attemptId);
  assert(fileReservation.reservation);
  const auto installed = store.install(*fileReservation.reservation, bytes);
  assert(installed.state == replay::ReplayInstallState::InstalledVerified &&
         installed.file);
  const ModernReplayFileAttachment attachment{
      .identity = reserved.reservation->identity,
      .metadata = installed.file->metadata};
  const auto staged = repository.StageModernChartResult(
      completed, std::nullopt, attachment, {});
  assert(staged.status == ModernChartStageStatus::Staged);
  const auto loaded =
      repository.LoadModernChartResultByAttempt(completed.attemptId);
  assert(loaded.status == ModernChartResultReadStatus::Loaded &&
         loaded.record && loaded.record->replayFile);
  return {.result = loaded.record->result,
          .reference = *loaded.record->replayFile};
}

void insertReservation(const std::filesystem::path &databasePath,
                       const ModernReplayPathReservation &reservation) {
  sqlite3 *database = nullptr;
  assert(sqlite3_open(databasePath.string().c_str(), &database) == SQLITE_OK);
  sqlite3_stmt *statement = nullptr;
  assert(sqlite3_prepare_v2(
             database,
             "INSERT INTO modern_replay_file_reservations("
             "attempt_id,stem,history_index,relative_path,created_at_unix_ms) "
             "VALUES(?,?,?,?,?)",
             -1, &statement, nullptr) == SQLITE_OK);
  const std::string relativePath =
      reservation.identity.relativePath.generic_string();
  assert(sqlite3_bind_text(statement, 1, reservation.attemptId.c_str(), -1,
                           SQLITE_TRANSIENT) == SQLITE_OK &&
         sqlite3_bind_text(statement, 2, reservation.identity.stem.c_str(),
                           -1, SQLITE_TRANSIENT) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 3,
                            reservation.identity.historyIndex) == SQLITE_OK &&
         sqlite3_bind_text(statement, 4, relativePath.c_str(), -1,
                           SQLITE_TRANSIENT) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 5,
                            reservation.createdAtUnixMillis) == SQLITE_OK &&
         sqlite3_step(statement) == SQLITE_DONE);
  sqlite3_finalize(statement);
  sqlite3_close(database);
}

void testVerifiedShareUsesStableSnapshotAndDeleteKeepsResult() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replays.db");
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);
  const auto installed = installResult(repository, store, 1, 'a');
  replay::ReplayFileActionService actions(repository, store);
  const replay::ReplayFileActionRequest request{
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = installed.result.attemptId};

  const auto inspected = actions.inspect(request);
  assert(inspected.state == replay::ReplayFileActionState::Verified);
  auto shared = actions.prepareShare(request);
  assert(shared.state == replay::ReplayFileActionState::Verified &&
         shared.share && std::filesystem::exists(shared.share->sourcePath) &&
         shared.share->suggestedFilename ==
             installed.reference.metadata.relativePath.filename().string());
  const auto original = temporary.path / installed.reference.metadata.relativePath;
  assert(shared.share->sourcePath != original);

  const auto removed = actions.remove(request);
  assert(removed.state == replay::ReplayFileActionState::UserDeleted &&
         removed.changed && !removed.cleanupPending &&
         !std::filesystem::exists(original) &&
         std::filesystem::exists(shared.share->sourcePath));
  const auto loaded =
      repository.LoadModernChartResultByAttempt(installed.result.attemptId);
  assert(loaded.status == ModernChartResultReadStatus::Loaded &&
         loaded.record && loaded.record->result == installed.result &&
         loaded.record->replayFile && loaded.record->replayFile->userDeleted);
  const auto repeated = actions.remove(request);
  assert(repeated.state == replay::ReplayFileActionState::UserDeleted &&
         !repeated.changed && !repeated.cleanupPending);
}

void testCorruptOwnedEntryRemainsDeletable() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replays.db");
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);
  const auto installed = installResult(repository, store, 2, 'b');
  const auto path = temporary.path / installed.reference.metadata.relativePath;
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "corrupt";
  }
  replay::ReplayFileActionService actions(repository, store);
  const replay::ReplayFileActionRequest request{
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = installed.result.attemptId};
  assert(actions.inspect(request).state ==
         replay::ReplayFileActionState::Corrupt);
  const auto removed = actions.remove(request);
  assert(removed.state == replay::ReplayFileActionState::UserDeleted &&
         removed.changed && !std::filesystem::exists(path));
}

void testListProbeDefersFullHashVerificationUntilAction() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replays.db");
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);
  const auto installed = installResult(repository, store, 9, 'f');
  const auto path = temporary.path / installed.reference.metadata.relativePath;
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const char replacement[4] = {0, 0, 0, 0};
    output.write(replacement, sizeof(replacement));
  }

  replay::ReplayFileActionService actions(repository, store);
  assert(actions.probe(installed.reference).state ==
             replay::ReplayFileActionState::Verified &&
         actions
                 .inspect({.owner = ModernReplayOwnerKind::ChartResult,
                           .attemptId = installed.result.attemptId})
                 .state == replay::ReplayFileActionState::Corrupt);
}

void testMissingFileDoesNotCreateADeletionTombstone() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replays.db");
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);
  const auto installed = installResult(repository, store, 3, 'c');
  std::filesystem::remove(temporary.path /
                          installed.reference.metadata.relativePath);
  replay::ReplayFileActionService actions(repository, store);
  const replay::ReplayFileActionRequest request{
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = installed.result.attemptId};

  const auto removed = actions.remove(request);
  assert(removed.state == replay::ReplayFileActionState::Missing &&
         !removed.changed);
  const auto loaded =
      repository.LoadModernChartResultByAttempt(installed.result.attemptId);
  assert(loaded.record && loaded.record->replayFile &&
         !loaded.record->replayFile->userDeleted);
}

void testResultMismatchedReferenceCannotBeInspectedOrDeleted() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replays.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);
  const auto installed = installResult(repository, store, 4, 'd');
  const auto deleted = installResult(repository, store, 5, 'e');
  replay::ReplayFileActionService deletionActions(repository, store);
  assert(deletionActions
             .remove({.owner = ModernReplayOwnerKind::ChartResult,
                      .attemptId = deleted.result.attemptId})
             .state == replay::ReplayFileActionState::UserDeleted);
  std::string diagnostic;
  const auto otherStem =
      replay::chartStem(std::string(64, 'f'), 1, false, diagnostic);
  const auto otherIdentity = replay::pathForStem(*otherStem, 0, diagnostic);
  std::filesystem::rename(
      temporary.path / installed.reference.metadata.relativePath,
      temporary.path / otherIdentity->relativePath);
  repository.Shutdown();
  sqlite3 *database = nullptr;
  assert(sqlite3_open(databasePath.string().c_str(), &database) == SQLITE_OK);
  sqlite3_stmt *statement = nullptr;
  assert(sqlite3_prepare_v2(
             database,
             "UPDATE modern_replay_files SET stem=?,history_index=?,"
             "relative_path=? WHERE id=?",
             -1, &statement, nullptr) == SQLITE_OK);
  assert(sqlite3_bind_text(statement, 1, otherIdentity->stem.c_str(), -1,
                           SQLITE_TRANSIENT) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 2, otherIdentity->historyIndex) ==
             SQLITE_OK);
  const std::string otherPath = otherIdentity->relativePath.generic_string();
  assert(sqlite3_bind_text(statement, 3, otherPath.c_str(), -1,
                           SQLITE_TRANSIENT) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 4, installed.reference.id) == SQLITE_OK &&
         sqlite3_step(statement) == SQLITE_DONE);
  sqlite3_finalize(statement);
  sqlite3_close(database);

  replay::ReplayFileActionService actions(repository, store);
  const replay::ReplayFileActionRequest request{
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = installed.result.attemptId};
  assert(actions.inspect(request).state ==
         replay::ReplayFileActionState::Invalid);
  const auto removed = actions.remove(request);
  assert(removed.state == replay::ReplayFileActionState::Invalid &&
         !removed.changed);
  const auto loaded =
      repository.LoadModernChartResultByAttempt(installed.result.attemptId);
  assert(loaded.record && loaded.record->replayFile &&
         !loaded.record->replayFile->userDeleted);
  const auto inventory =
      replay::loadAgreedModernReplayFileInventory(repository);
  assert(inventory.status ==
         ModernReplayFileInventoryStatus::IntegrityConflict);
  const auto tombstones =
      replay::loadAgreedModernReplayTombstoneInventory(repository);
  assert(tombstones.status == ModernReplayFileInventoryStatus::Loaded &&
         tombstones.entries.size() == 1 &&
         tombstones.entries.front().attemptId == deleted.result.attemptId &&
         tombstones.entries.front().reference.userDeleted);
}

void testUnsupportedCodecSkipsMaterializationAndRemainsDeletable() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replays.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);
  const auto installed = installResult(repository, store, 5, 'e');
  const auto path = temporary.path / installed.reference.metadata.relativePath;
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "would be corrupt if inspected";
  }
  repository.Shutdown();
  sqlite3 *database = nullptr;
  assert(sqlite3_open(databasePath.string().c_str(), &database) == SQLITE_OK);
  const std::string update =
      "UPDATE modern_replay_files SET codec_version=" +
      std::to_string(replay::BeatorajaReplayCodec::kCodecVersion + 1) +
      " WHERE id=" + std::to_string(installed.reference.id);
  assert(sqlite3_exec(database, update.c_str(), nullptr, nullptr, nullptr) ==
         SQLITE_OK);
  sqlite3_close(database);

  replay::ReplayFileActionService actions(repository, store);
  const replay::ReplayFileActionRequest request{
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = installed.result.attemptId};
  assert(actions.inspect(request).state ==
         replay::ReplayFileActionState::UnsupportedCodecVersion);
  const auto shared = actions.prepareShare(request);
  assert(shared.state ==
             replay::ReplayFileActionState::UnsupportedCodecVersion &&
         !shared.share);
  const auto removed = actions.remove(request);
  assert(removed.state == replay::ReplayFileActionState::UserDeleted &&
         removed.changed && !std::filesystem::exists(path));
}

void testReservationInventoryChecksWhetherChartStagingCommitted() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replays.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);

  const auto orphan = result(6, 'a');
  const auto orphanReservation = repository.ReserveModernReplayPath(
      orphan.attemptId, orphan.score.chartSha256, orphan.playedAtUnixMillis);
  assert(orphanReservation.reservation);

  const auto resultOnly = result(7, 'b');
  const auto resultOnlyReservation = repository.ReserveModernReplayPath(
      resultOnly.attemptId, resultOnly.score.chartSha256,
      resultOnly.playedAtUnixMillis);
  assert(resultOnlyReservation.reservation &&
         repository
                 .StageModernChartResult(resultOnly, std::nullopt,
                                         std::nullopt, {})
                 .status == ModernChartStageStatus::Staged);

  const auto attached = installResult(repository, store, 8, 'c');
  const ModernReplayPathReservation retainedAfterCommit{
      .attemptId = attached.result.attemptId,
      .identity = attached.reference.identity,
      .createdAtUnixMillis = attached.result.playedAtUnixMillis};
  repository.Shutdown();
  insertReservation(databasePath, retainedAfterCommit);

  const auto inventory =
      replay::loadAgreedModernReplayPathReservationInventory(repository);
  const auto stateFor = [&](std::string_view attemptId) {
    const auto found = std::ranges::find_if(
        inventory.entries, [&](const auto &entry) {
          return entry.reservation.attemptId == attemptId;
        });
    assert(found != inventory.entries.end());
    return found->commitState;
  };
  assert(inventory.status == ModernReplayFileInventoryStatus::Loaded &&
         inventory.entries.size() == 3 &&
         stateFor(orphan.attemptId) ==
             replay::ModernReplayReservationCommitState::Unassociated &&
         stateFor(resultOnly.attemptId) ==
             replay::ModernReplayReservationCommitState::ResultOnly &&
         stateFor(attached.result.attemptId) ==
             replay::ModernReplayReservationCommitState::ReplayAttached);
}

void testRestartReconciliationRemovesOnlyUnattachedFinalFiles() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replays.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);

  const auto installUnattached = [&](int suffix, char hash) {
    const auto completed = result(suffix, hash);
    const auto reserved = repository.ReserveModernReplayPath(
        completed.attemptId, completed.score.chartSha256,
        completed.playedAtUnixMillis);
    assert(reserved.reservation);
    const std::vector bytes{
        std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08},
        std::byte{static_cast<unsigned char>(suffix)}};
    const auto fileReservation = store.reserve(
        reserved.reservation->identity, bytes, completed.attemptId);
    assert(fileReservation.reservation);
    const auto installed =
        store.install(*fileReservation.reservation, bytes);
    assert(installed.state == replay::ReplayInstallState::InstalledVerified &&
           installed.file && installed.file->lifecycle.receipt);
    const auto owned = repository.RecordModernReplayInstallIntent(
        *reserved.reservation, *installed.file->lifecycle.receipt);
    assert(owned.status == ModernReplayOwnershipRecordStatus::Recorded &&
           owned.reservation && owned.reservation->ownedFile);
    return std::tuple{completed, *owned.reservation,
                      installed.file->metadata};
  };

  const auto [orphanResult, orphanReservation, orphanMetadata] =
      installUnattached(1, 'a');
  const auto [resultOnly, resultOnlyReservation, resultOnlyMetadata] =
      installUnattached(2, 'b');
  assert(repository
             .StageModernChartResult(resultOnly, std::nullopt, std::nullopt,
                                     {})
             .status == ModernChartStageStatus::Staged);
  const auto attached = installResult(repository, store, 3, 'c');
  const ModernReplayPathReservation retainedAfterCommit{
      .attemptId = attached.result.attemptId,
      .identity = attached.reference.identity,
      .createdAtUnixMillis = attached.result.playedAtUnixMillis};
  repository.Shutdown();
  insertReservation(databasePath, retainedAfterCommit);

  replay::ReplayFileReconciler reconciler({
      .listTombstones = [&] {
        return replay::loadAgreedModernReplayTombstoneInventory(repository);
      },
      .removeTombstonedEntryIfMatches =
          [&](const replay::ReplayFileMetadata &metadata,
              std::string &diagnostic) {
            return store.removeIfMatches(metadata, diagnostic);
          },
      .removeStaleTemporaryFiles =
          [&](auto cutoff) { store.removeStaleTemporaryFiles(cutoff); },
      .listReservations = [&] {
        return replay::loadAgreedModernReplayPathReservationInventory(
            repository);
      },
      .removeOwnedReservedEntry =
          [&](const replay::ReplayFileMetadata &metadata,
              std::string &diagnostic) {
            return store.removeIfMatches(metadata, diagnostic);
          },
      .releaseReservation = [&](const auto &reservation) {
        return repository.ReleaseModernReplayPathReservation(reservation);
      },
  });
  const auto report = reconciler.reconcile(std::chrono::system_clock::now());

  assert(report.failures.empty() && report.reservationsScanned == 3 &&
         report.unassociatedReservationsFound == 2 &&
         report.attachedReservationsFound == 1 &&
         report.unassociatedFilesRemoved == 2 &&
         report.reservationsReleased == 3 &&
         !std::filesystem::exists(temporary.path /
                                  orphanMetadata.relativePath) &&
         !std::filesystem::exists(temporary.path /
                                  resultOnlyMetadata.relativePath) &&
         std::filesystem::exists(
             temporary.path / attached.reference.metadata.relativePath) &&
         repository.ListModernReplayPathReservations().reservations.empty());
  const auto preservedResultOnly =
      repository.LoadModernChartResultByAttempt(resultOnly.attemptId);
  const auto preservedAttached =
      repository.LoadModernChartResultByAttempt(attached.result.attemptId);
  assert(repository.LoadModernChartResultByAttempt(orphanResult.attemptId)
                 .status == ModernChartResultReadStatus::NotFound &&
         preservedResultOnly.status == ModernChartResultReadStatus::Loaded &&
         preservedResultOnly.record &&
         !preservedResultOnly.record->replayFile &&
         preservedAttached.status == ModernChartResultReadStatus::Loaded &&
         preservedAttached.record && preservedAttached.record->replayFile);
}

void testCrashAfterFinalInstallUsesPreinstalledOwnershipJournal() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replays.db");
  assert(repository.EnsureSchema());
  auto completed = result(5, 'a');
  const std::vector bytes{std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08},
                          std::byte{0x2a}};
  bool interrupted = false;
  auto store = std::make_shared<replay::ReplayFileStore>(
      temporary.path,
      replay::ReplayFileStoreFaults{
          .failAt = [&](std::string_view point) {
            if (point == "after-install") {
              interrupted = true;
              return true;
            }
            return false;
          }});
  replay::ReplayFileAssociationCoordinator coordinator({
      .reservePath =
          [&](std::string_view attemptId, std::string_view stem,
              std::int64_t playedAt) {
            return repository.ReserveModernReplayPath(attemptId, stem,
                                                      playedAt);
          },
      .releasePath = [&](const auto &reservation) {
        return repository.ReleaseModernReplayPathReservation(reservation);
      },
      .reserveFile =
          [store](const auto &identity, std::span<const std::byte> payload,
                  std::string_view attemptId) {
            return store->reserve(identity, payload, attemptId);
          },
      .installFile =
          [store](const auto &reservation,
                  std::span<const std::byte> payload,
                  const replay::ReplayInstallOwnershipJournal &journal) {
            return store->install(reservation, payload, journal);
          },
      .recordInstallIntent = [&](const auto &reservation,
                                 const auto &receipt) {
        return repository.RecordModernReplayInstallIntent(reservation,
                                                          receipt);
      },
      .inspectFile =
          [store](const auto &metadata) { return store->inspect(metadata); },
      .removeIfMatches =
          [store](const auto &metadata, std::string &diagnostic) {
            return store->removeIfMatches(metadata, diagnostic);
          },
  });
  const auto associated = coordinator.associate(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis,
      [&](std::string &) { return std::optional(bytes); });
  assert(interrupted &&
         associated.status == replay::ReplayFileAssociationStatus::Attached &&
         associated.association &&
         associated.association->ownership ==
             replay::ReplayFileInstalledOwnership::Ambiguous);

  const auto pending = repository.ListModernReplayPathReservations();
  assert(pending.status == ModernReplayFileInventoryStatus::Loaded &&
         pending.reservations.size() == 1 &&
         pending.reservations.front().ownedFile.has_value());
  const auto installedPath = temporary.path /
                             pending.reservations.front().identity.relativePath;
  assert(std::filesystem::exists(installedPath));

  replay::ReplayFileStore restartStore(temporary.path);
  replay::ReplayFileReconciler reconciler({
      .listTombstones = [&] {
        return replay::loadAgreedModernReplayTombstoneInventory(repository);
      },
      .removeTombstonedEntryIfMatches =
          [&](const auto &metadata, std::string &diagnostic) {
            return restartStore.removeIfMatches(metadata, diagnostic);
          },
      .removeStaleTemporaryFiles =
          [&](auto cutoff) { restartStore.removeStaleTemporaryFiles(cutoff); },
      .listReservations = [&] {
        return replay::loadAgreedModernReplayPathReservationInventory(
            repository);
      },
      .removeOwnedReservedEntry =
          [&](const auto &metadata, std::string &diagnostic) {
            return restartStore.removeIfMatches(metadata, diagnostic);
          },
      .releaseReservation = [&](const auto &reservation) {
        return repository.ReleaseModernReplayPathReservation(reservation);
      },
  });
  const auto report = reconciler.reconcile(std::chrono::system_clock::now());
  assert(report.failures.empty() &&
         report.unassociatedFilesRemoved == 1 &&
         report.reservationsReleased == 1 &&
         !std::filesystem::exists(installedPath));
}

void testRestartReconciliationReleasesPathOnlyReservationWithoutDeletingOccupant() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path / "replays.db";
  ReplayRepository repository(databasePath);
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);

  const auto completed = result(9, 'd');
  const auto reserved = repository.ReserveModernReplayPath(
      completed.attemptId, completed.score.chartSha256,
      completed.playedAtUnixMillis);
  assert(reserved.reservation);
  const auto occupiedPath =
      temporary.path / reserved.reservation->identity.relativePath;
  std::filesystem::create_directories(occupiedPath.parent_path());
  {
    std::ofstream output(occupiedPath, std::ios::binary | std::ios::trunc);
    output << "pre-existing unrelated replay bytes";
  }

  replay::ReplayFileReconciler reconciler({
      .listTombstones = [&] {
        return replay::loadAgreedModernReplayTombstoneInventory(repository);
      },
      .removeTombstonedEntryIfMatches =
          [&](const replay::ReplayFileMetadata &metadata,
              std::string &diagnostic) {
            return store.removeIfMatches(metadata, diagnostic);
          },
      .removeStaleTemporaryFiles =
          [&](auto cutoff) { store.removeStaleTemporaryFiles(cutoff); },
      .listReservations = [&] {
        return replay::loadAgreedModernReplayPathReservationInventory(
            repository);
      },
      .removeOwnedReservedEntry =
          [&](const replay::ReplayFileMetadata &metadata,
              std::string &diagnostic) {
            return store.removeIfMatches(metadata, diagnostic);
          },
      .releaseReservation = [&](const auto &reservation) {
        return repository.ReleaseModernReplayPathReservation(reservation);
      },
  });
  const auto report = reconciler.reconcile(std::chrono::system_clock::now());

  assert(report.failures.empty() && report.reservationsScanned == 1 &&
         report.unassociatedReservationsFound == 1 &&
         report.unassociatedFilesRemoved == 0 &&
         report.reservationsReleased == 1 &&
         std::filesystem::exists(occupiedPath) &&
         repository.ListModernReplayPathReservations().reservations.empty());
  std::ifstream input(occupiedPath, std::ios::binary);
  const std::string preserved((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
  assert(preserved == "pre-existing unrelated replay bytes");
}

void testRestartTombstoneCleanupPreservesReplacementBytes() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replays.db");
  assert(repository.EnsureSchema());
  replay::ReplayFileStore store(temporary.path);
  const auto installed = installResult(repository, store, 4, 'f');
  const auto tombstoned = repository.MarkModernReplayFileUserDeleted(
      ModernReplayOwnerKind::ChartResult, installed.result.attemptId,
      installed.reference);
  assert(tombstoned.status == ModernReplayFileMutationStatus::Changed);

  const auto path = temporary.path / installed.reference.metadata.relativePath;
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "replacement replay bytes";
  }

  replay::ReplayFileReconciler reconciler({
      .listTombstones = [&] {
        return replay::loadAgreedModernReplayTombstoneInventory(repository);
      },
      .removeTombstonedEntryIfMatches =
          [&](const replay::ReplayFileMetadata &metadata,
              std::string &diagnostic) {
            return store.removeIfMatches(metadata, diagnostic);
          },
      .removeStaleTemporaryFiles =
          [&](auto cutoff) { store.removeStaleTemporaryFiles(cutoff); },
      .listReservations = [] {
        return replay::ModernReplayReservationReconciliationOutcome{
            .status = ModernReplayFileInventoryStatus::Loaded};
      },
  });
  const auto report = reconciler.reconcile(std::chrono::system_clock::now());

  assert(report.tombstonesFound == 1 && report.filesRemoved == 0 &&
         report.failures.size() == 1 && std::filesystem::exists(path));
  std::ifstream input(path, std::ios::binary);
  const std::string preserved((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
  assert(preserved == "replacement replay bytes");
}

void testInteractiveDeletionPreservesReplacementAfterInspection() {
  TemporaryDirectory temporary;
  ReplayRepository repository(temporary.path / "replays.db");
  assert(repository.EnsureSchema());
  replay::ReplayFileStore installStore(temporary.path);
  const auto installed = installResult(repository, installStore, 9, 'f');
  const auto path = temporary.path / installed.reference.metadata.relativePath;

  bool replaced = false;
  replay::ReplayFileStore actionStore(
      temporary.path,
      {.failAt = [&](std::string_view point) {
        if (!replaced && point == "remove-before-quarantine") {
          std::ofstream output(path, std::ios::binary | std::ios::trunc);
          output << "replacement replay bytes";
          replaced = true;
        }
        return false;
      }});
  replay::ReplayFileActionService actions(repository, actionStore);
  const auto removed = actions.remove({
      .owner = ModernReplayOwnerKind::ChartResult,
      .attemptId = installed.result.attemptId,
  });

  assert(replaced &&
         removed.state == replay::ReplayFileActionState::UserDeleted &&
         removed.changed && removed.cleanupPending &&
         std::filesystem::exists(path));
  std::ifstream input(path, std::ios::binary);
  const std::string preserved((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
  assert(preserved == "replacement replay bytes");
}

void testActionInspectionProjectsRecordCapabilitiesWithoutMaterialization() {
  using replay::ReplayFileActionState;
  using replay::ReplayState;
  assert(replay::replayStateForFileAction(ReplayFileActionState::Verified) ==
         ReplayState::Verified);
  assert(replay::replayStateForFileAction(ReplayFileActionState::UserDeleted) ==
         ReplayState::UserDeleted);
  assert(replay::replayStateForFileAction(ReplayFileActionState::Missing) ==
         ReplayState::Missing);
  assert(replay::replayStateForFileAction(ReplayFileActionState::IoFailure) ==
         ReplayState::Missing);
  assert(replay::replayStateForFileAction(ReplayFileActionState::Corrupt) ==
         ReplayState::Corrupt);
  assert(replay::replayStateForFileAction(ReplayFileActionState::Mismatched) ==
         ReplayState::Mismatched);
  assert(replay::replayStateForFileAction(
             ReplayFileActionState::UnsupportedCodecVersion) ==
         ReplayState::UnsupportedExtension);
  assert(replay::replayStateForFileAction(ReplayFileActionState::Invalid) ==
         ReplayState::NotApplicable);
  assert(replay::replayStateForFileAction(ReplayFileActionState::ResultNotFound) ==
         ReplayState::NotApplicable);
}

} // namespace

int main() {
  testVerifiedShareUsesStableSnapshotAndDeleteKeepsResult();
  testCorruptOwnedEntryRemainsDeletable();
  testListProbeDefersFullHashVerificationUntilAction();
  testMissingFileDoesNotCreateADeletionTombstone();
  testResultMismatchedReferenceCannotBeInspectedOrDeleted();
  testUnsupportedCodecSkipsMaterializationAndRemainsDeletable();
  testReservationInventoryChecksWhetherChartStagingCommitted();
  testRestartReconciliationRemovesOnlyUnattachedFinalFiles();
  testCrashAfterFinalInstallUsesPreinstalledOwnershipJournal();
  testRestartReconciliationReleasesPathOnlyReservationWithoutDeletingOccupant();
  testRestartTombstoneCleanupPreservesReplacementBytes();
  testInteractiveDeletionPreservesReplacementAfterInspection();
  testActionInspectionProjectsRecordCapabilitiesWithoutMaterialization();
  return 0;
}
