#include "ReplayProfileTransfer.h"

#include "BeatorajaReplayPath.h"
#include "ReplayFileStore.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace replay {
namespace {

bool fault(const ReplayProfileTransferFaults &faults, std::string_view point) {
  return faults.failAt && faults.failAt(point);
}

bool validReference(const ModernReplayFileInventoryEntry &entry,
                    std::string &diagnostic) {
  if (entry.reference.id <= 0 || entry.reference.resultId <= 0 ||
      entry.attemptId.empty() || entry.reference.identity.stem.empty() ||
      entry.reference.identity.historyIndex < 0 ||
      entry.reference.identity.relativePath !=
          entry.reference.metadata.relativePath) {
    diagnostic = "Replay inventory entry has inconsistent ownership";
    return false;
  }
  const auto expected = pathForStem(entry.reference.identity.stem,
                                    entry.reference.identity.historyIndex,
                                    diagnostic);
  if (!expected || *expected != entry.reference.identity) {
    if (diagnostic.empty()) {
      diagnostic = "Replay inventory path does not match its identity";
    }
    return false;
  }
  return true;
}

void rollbackCopied(const std::filesystem::path &destinationRoot,
                    const std::vector<ReplayFileMetadata> &installed) {
  ReplayFileStore store(destinationRoot);
  for (auto iterator = installed.rbegin(); iterator != installed.rend();
       ++iterator) {
    std::string ignored;
    store.removeIfMatches(*iterator, ignored);
  }
}

ReplayProfileTransferOutcome invalidSource(std::string diagnostic) {
  return {.state = ReplayProfileTransferState::SourceInvalid,
          .diagnostic = std::move(diagnostic)};
}

ReplayProfileTransferOutcome invalidDestination(std::string diagnostic) {
  return {.state = ReplayProfileTransferState::DestinationInvalid,
          .diagnostic = std::move(diagnostic)};
}

} // namespace

ReplayProfileTransfer::ReplayProfileTransfer(
    std::filesystem::path sourceProfileRoot,
    std::filesystem::path destinationProfileRoot,
    ReplayProfileTransferFaults faults)
    : sourceProfileRoot_(std::move(sourceProfileRoot)),
      destinationProfileRoot_(std::move(destinationProfileRoot)),
      faults_(std::move(faults)) {}

ReplayProfileTransferOutcome ReplayProfileTransfer::copy(
    const std::vector<ModernReplayFileInventoryEntry> &inventory) const {
  ReplayFileStore source(sourceProfileRoot_);
  ReplayFileStore destination(destinationProfileRoot_);
  ReplayProfileTransferOutcome outcome{
      .state = ReplayProfileTransferState::Succeeded};
  std::vector<ReplayFileMetadata> newlyInstalled;
  auto fail = [&](ReplayProfileTransferState state, std::string diagnostic) {
    rollbackCopied(destinationProfileRoot_, newlyInstalled);
    outcome.state = state;
    outcome.copiedRelativePaths.clear();
    outcome.diagnostic = std::move(diagnostic);
    return outcome;
  };

  for (const auto &entry : inventory) {
    std::string diagnostic;
    if (!validReference(entry, diagnostic)) {
      return fail(ReplayProfileTransferState::SourceInvalid,
                  std::move(diagnostic));
    }
    if (entry.reference.userDeleted) {
      ++outcome.omittedUserDeleted;
      continue;
    }
    if (fault(faults_, "before-copy")) {
      return fail(ReplayProfileTransferState::DestinationFailure,
                  "Injected replay transfer failure before copy");
    }
    const auto read = source.readVerified(entry.reference.metadata);
    if (read.state == ReplayFileState::Missing) {
      ++outcome.omittedMissing;
      continue;
    }
    if (read.state != ReplayFileState::Available || !read.bytes) {
      return fail(ReplayProfileTransferState::SourceInvalid,
                  read.diagnostic.empty()
                      ? "Referenced replay file is not transferable"
                      : read.diagnostic);
    }
    const auto reservation = destination.reserve(
        entry.reference.identity, *read.bytes, entry.attemptId);
    if (!reservation.reservation ||
        reservation.reservation->expectedMetadata !=
            entry.reference.metadata) {
      return fail(ReplayProfileTransferState::DestinationFailure,
                  reservation.diagnostic.empty()
                      ? "Replay metadata cannot be preserved at destination"
                      : reservation.diagnostic);
    }
    const auto installed =
        destination.install(*reservation.reservation, *read.bytes);
    if (installed.state != ReplayInstallState::InstalledVerified ||
        !installed.file ||
        installed.file->metadata != entry.reference.metadata) {
      return fail(ReplayProfileTransferState::DestinationFailure,
                  installed.diagnostic.empty()
                      ? "Replay file could not be installed at destination"
                      : installed.diagnostic);
    }
    if (!installed.existingIdenticalFile) {
      newlyInstalled.push_back(installed.file->metadata);
    }
    outcome.copiedRelativePaths.push_back(
        entry.reference.metadata.relativePath);
    if (fault(faults_, "after-copy")) {
      return fail(ReplayProfileTransferState::DestinationFailure,
                  "Injected replay transfer failure after copy");
    }
  }

  if (fault(faults_, "after-validate")) {
    return fail(ReplayProfileTransferState::DestinationFailure,
                "Injected replay transfer failure after validation");
  }
  return outcome;
}

ReplayProfileTransferOutcome ReplayProfileTransfer::validate(
    const std::vector<ModernReplayFileInventoryEntry> &inventory,
    bool rejectUnreferencedFiles) const {
  ReplayFileStore destination(destinationProfileRoot_);
  std::set<std::string, std::less<>> activePaths;
  ReplayProfileTransferOutcome outcome{
      .state = ReplayProfileTransferState::Succeeded};
  for (const auto &entry : inventory) {
    std::string diagnostic;
    if (!validReference(entry, diagnostic)) {
      return invalidDestination(std::move(diagnostic));
    }
    const auto inspected = destination.inspect(entry.reference.metadata);
    if (entry.reference.userDeleted) {
      ++outcome.omittedUserDeleted;
      if (inspected.state != ReplayFileState::Missing &&
          rejectUnreferencedFiles) {
        return invalidDestination(
            "User-deleted replay bytes are present in destination");
      }
      continue;
    }
    activePaths.insert(entry.reference.metadata.relativePath.generic_string());
    if (inspected.state == ReplayFileState::Missing) {
      ++outcome.omittedMissing;
      continue;
    }
    if (inspected.state != ReplayFileState::Available) {
      return invalidDestination(
          inspected.diagnostic.empty()
              ? "Referenced destination replay file is invalid"
              : inspected.diagnostic);
    }
    outcome.copiedRelativePaths.push_back(
        entry.reference.metadata.relativePath);
  }

  if (!rejectUnreferencedFiles) {
    return outcome;
  }
  const auto replayDirectory = destinationProfileRoot_ / "replay";
  std::error_code error;
  const auto status = std::filesystem::symlink_status(replayDirectory, error);
  if (error == std::errc::no_such_file_or_directory ||
      status.type() == std::filesystem::file_type::not_found) {
    return outcome;
  }
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    return invalidDestination("Destination replay directory is unsafe");
  }
  std::filesystem::directory_iterator iterator(replayDirectory, error);
  if (error) {
    return invalidDestination("Destination replay directory cannot be read");
  }
  for (const auto &entry : iterator) {
    const auto entryStatus = std::filesystem::symlink_status(entry.path(), error);
    const auto relative =
        (std::filesystem::path("replay") / entry.path().filename())
            .generic_string();
    if (error || !std::filesystem::is_regular_file(entryStatus) ||
        std::filesystem::is_symlink(entryStatus) ||
        !activePaths.contains(relative)) {
      return invalidDestination(
          "Destination contains an unowned or unsafe replay entry");
    }
  }
  return outcome;
}

} // namespace replay
