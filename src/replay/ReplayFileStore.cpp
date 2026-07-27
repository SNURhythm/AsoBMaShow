#include "ReplayFileStore.h"

#include "BeatorajaReplayCodec.h"
#include "../AtomicFile.h"
#include "../FileChecksum.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <ranges>
#include <system_error>

namespace replay {
namespace {

constexpr std::string_view kTemporaryPrefix = ".asobmashow-replay-";
constexpr std::size_t kAttemptDigestBytes = 64;
constexpr std::size_t kContentDigestPrefixBytes = 16;
constexpr std::string_view kTemporarySuffix = ".tmp";

bool canonicalHex(std::string_view value, std::size_t size) noexcept {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

std::string hashBytes(std::span<const std::byte> bytes) {
  return file_checksum::sha256(
      {reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}

bool canonicalIdentity(const ReplayPathIdentity &identity,
                       const ReplayLimits &limits) {
  std::string diagnostic;
  const auto rebuilt =
      pathForStem(identity.stem, identity.historyIndex, diagnostic, limits);
  return rebuilt && *rebuilt == identity;
}

bool safeRelativePath(const std::filesystem::path &path,
                      const ReplayLimits &limits) {
  if (path.empty() || path.is_absolute() || path.has_root_path() ||
      path.lexically_normal() != path || path.parent_path() != "replay") {
    return false;
  }
  const std::string filename = path.filename().string();
  return !filename.empty() && filename.size() <= limits.maxFilenameBytes &&
         filename.ends_with(".brd") &&
         filename.find('/') == std::string::npos &&
         filename.find('\\') == std::string::npos;
}

bool safeTemporaryRelativePath(const std::filesystem::path &path) {
  return !path.empty() && !path.is_absolute() && !path.has_root_path() &&
         path.lexically_normal() == path && path.parent_path() == "replay" &&
         isPrivateReplayTemporaryFilename(path.filename().string());
}

enum class EntryKind { Missing, Regular, Directory, Symlink, Other, Error };

EntryKind entryKind(const std::filesystem::path &path,
                    std::string &diagnostic) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    return EntryKind::Missing;
  }
  if (error) {
    diagnostic = "Unable to inspect replay path '" + path.string() +
                 "': " + error.message();
    return EntryKind::Error;
  }
  switch (status.type()) {
  case std::filesystem::file_type::not_found:
    return EntryKind::Missing;
  case std::filesystem::file_type::regular:
    return EntryKind::Regular;
  case std::filesystem::file_type::directory:
    return EntryKind::Directory;
  case std::filesystem::file_type::symlink:
    return EntryKind::Symlink;
  default:
    return EntryKind::Other;
  }
}

bool ensureReplayDirectory(const std::filesystem::path &profileRoot,
                           std::filesystem::path &replayDirectory,
                           std::string &diagnostic) {
  const auto rootKind = entryKind(profileRoot, diagnostic);
  if (rootKind != EntryKind::Directory) {
    if (diagnostic.empty()) {
      diagnostic = "Replay profile root is missing or unsafe";
    }
    return false;
  }
  replayDirectory = profileRoot / "replay";
  const auto replayKind = entryKind(replayDirectory, diagnostic);
  if (replayKind == EntryKind::Directory) {
    return true;
  }
  if (replayKind != EntryKind::Missing) {
    if (diagnostic.empty()) {
      diagnostic = "Replay directory is not a real directory";
    }
    return false;
  }
  std::error_code error;
  if (!std::filesystem::create_directory(replayDirectory, error) || error) {
    diagnostic = "Unable to create replay directory: " + error.message();
    return false;
  }
  if (!atomic_file::syncDirectory(profileRoot, diagnostic)) {
    return false;
  }
  return true;
}

bool existingReplayDirectory(const std::filesystem::path &profileRoot,
                             std::filesystem::path &replayDirectory,
                             ReplayFileState &absentState,
                             std::string &diagnostic) {
  const auto rootKind = entryKind(profileRoot, diagnostic);
  if (rootKind == EntryKind::Missing) {
    absentState = ReplayFileState::Missing;
    return false;
  }
  if (rootKind == EntryKind::Error) {
    absentState = ReplayFileState::IoFailure;
    return false;
  }
  if (rootKind != EntryKind::Directory) {
    absentState = ReplayFileState::Unsafe;
    diagnostic = "Replay profile root is unsafe";
    return false;
  }
  replayDirectory = profileRoot / "replay";
  const auto replayKind = entryKind(replayDirectory, diagnostic);
  if (replayKind == EntryKind::Missing) {
    absentState = ReplayFileState::Missing;
    return false;
  }
  if (replayKind == EntryKind::Error) {
    absentState = ReplayFileState::IoFailure;
    return false;
  }
  if (replayKind != EntryKind::Directory) {
    absentState = ReplayFileState::Unsafe;
    diagnostic = "Replay directory is unsafe";
    return false;
  }
  return true;
}

bool fault(const ReplayFileStoreFaults &faults, std::string_view point) {
  return faults.failAt && faults.failAt(point);
}

ReplayFileInspection inspectAt(const std::filesystem::path &profileRoot,
                               const ReplayFileMetadata &metadata,
                               const ReplayLimits &limits) {
  ReplayFileInspection outcome;
  if (!safeRelativePath(metadata.relativePath, limits) ||
      !canonicalHex(metadata.sha256, 64) || metadata.compressedSize == 0 ||
      metadata.compressedSize > limits.maxCompressedBytes ||
      metadata.codecVersion <= 0) {
    outcome.state = ReplayFileState::Unsafe;
    outcome.diagnostic = "Replay metadata is unsafe";
    return outcome;
  }
  std::filesystem::path replayDirectory;
  ReplayFileState absentState = ReplayFileState::IoFailure;
  if (!existingReplayDirectory(profileRoot, replayDirectory, absentState,
                               outcome.diagnostic)) {
    outcome.state = absentState;
    return outcome;
  }
  const auto path = profileRoot / metadata.relativePath;
  const auto kind = entryKind(path, outcome.diagnostic);
  if (kind == EntryKind::Missing) {
    outcome.state = ReplayFileState::Missing;
    return outcome;
  }
  if (kind == EntryKind::Error) {
    outcome.state = ReplayFileState::IoFailure;
    return outcome;
  }
  if (kind != EntryKind::Regular) {
    outcome.state = ReplayFileState::Unsafe;
    outcome.diagnostic = "Replay file is not a regular file";
    return outcome;
  }
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    outcome.state = ReplayFileState::IoFailure;
    outcome.diagnostic = "Unable to read replay size: " + error.message();
    return outcome;
  }
  if (size != metadata.compressedSize) {
    outcome.state = ReplayFileState::Corrupt;
    outcome.diagnostic = "Replay file size does not match metadata";
    return outcome;
  }
  std::string checksumError;
  const auto checksum =
      file_checksum::sha256File(path, checksumError, limits.maxCompressedBytes);
  if (!checksum) {
    outcome.state = ReplayFileState::IoFailure;
    outcome.diagnostic = std::move(checksumError);
    return outcome;
  }
  if (*checksum != metadata.sha256) {
    outcome.state = ReplayFileState::Corrupt;
    outcome.diagnostic = "Replay file hash does not match metadata";
    return outcome;
  }
  outcome.state = ReplayFileState::Available;
  return outcome;
}

std::optional<ReplayInstalledFile>
installedFile(const ReplayFileReservation &reservation,
              std::string &diagnostic) {
  ReplayFileLifecycle lifecycle =
      reservedReplayFileLifecycle(reservation.attemptToken);
  auto transition = advanceReplayFileLifecycle(
      lifecycle, ReplayFileLifecycleEvent::TemporaryWritten);
  if (!transition.lifecycle) {
    diagnostic = std::move(transition.diagnostic);
    return std::nullopt;
  }
  const ReplayFileOwnershipReceipt receipt{
      .attemptToken = reservation.attemptToken,
      .metadata = reservation.expectedMetadata,
  };
  transition = advanceReplayFileLifecycle(
      *transition.lifecycle, ReplayFileLifecycleEvent::InstallVerified,
      receipt);
  if (!transition.lifecycle) {
    diagnostic = std::move(transition.diagnostic);
    return std::nullopt;
  }
  return ReplayInstalledFile{
      .metadata = reservation.expectedMetadata,
      .attemptToken = reservation.attemptToken,
      .lifecycle = std::move(*transition.lifecycle),
  };
}

bool removeEntryAndSync(const std::filesystem::path &path,
                        const std::filesystem::path &directory,
                        std::string &diagnostic) {
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error) {
    diagnostic = "Unable to remove replay entry: " + error.message();
    return false;
  }
  if (removed && !atomic_file::syncDirectory(directory, diagnostic)) {
    return false;
  }
  return true;
}

} // namespace

bool isPrivateReplayTemporaryFilename(std::string_view filename) noexcept {
  const std::size_t expected = kTemporaryPrefix.size() + kAttemptDigestBytes +
                               1 + kContentDigestPrefixBytes +
                               kTemporarySuffix.size();
  if (filename.size() != expected || !filename.starts_with(kTemporaryPrefix) ||
      !filename.ends_with(kTemporarySuffix)) {
    return false;
  }
  filename.remove_prefix(kTemporaryPrefix.size());
  const std::string_view attempt = filename.substr(0, kAttemptDigestBytes);
  filename.remove_prefix(kAttemptDigestBytes);
  if (filename.empty() || filename.front() != '-') {
    return false;
  }
  filename.remove_prefix(1);
  const std::string_view content =
      filename.substr(0, kContentDigestPrefixBytes);
  filename.remove_prefix(kContentDigestPrefixBytes);
  return canonicalHex(attempt, kAttemptDigestBytes) &&
         canonicalHex(content, kContentDigestPrefixBytes) &&
         filename == kTemporarySuffix;
}

ReplayFileStore::ReplayFileStore(std::filesystem::path profileRoot,
                                 ReplayFileStoreFaults faults,
                                 ReplayLimits limits)
    : faults_(std::move(faults)), limits_(limits) {
  std::error_code error;
  profileRoot_ = std::filesystem::absolute(profileRoot, error);
  if (error) {
    profileRoot_ = std::move(profileRoot);
  }
  profileRoot_ = profileRoot_.lexically_normal();
}

ReplayReservationOutcome
ReplayFileStore::reserve(const ReplayPathIdentity &identity,
                         std::span<const std::byte> bytes,
                         std::string_view attemptToken) const {
  ReplayReservationOutcome outcome;
  if (fault(faults_, "reservation")) {
    outcome.diagnostic = "Injected replay reservation failure";
    return outcome;
  }
  if (!limits_.valid() || !canonicalIdentity(identity, limits_) ||
      attemptToken.empty() || attemptToken.size() > limits_.maxStringBytes ||
      bytes.empty() || bytes.size() > limits_.maxCompressedBytes) {
    outcome.diagnostic = "Replay reservation input is invalid";
    return outcome;
  }
  const std::string contentHash = hashBytes(bytes);
  const std::string attemptHash = file_checksum::sha256(attemptToken);
  const std::string temporaryName =
      std::string(kTemporaryPrefix) + attemptHash + "-" +
      contentHash.substr(0, kContentDigestPrefixBytes) +
      std::string(kTemporarySuffix);
  outcome.reservation = ReplayFileReservation{
      .identity = identity,
      .attemptToken = std::string(attemptToken),
      .expectedMetadata = {.relativePath = identity.relativePath,
                           .sha256 = contentHash,
                           .compressedSize =
                               static_cast<std::uint64_t>(bytes.size()),
                           .codecVersion = BeatorajaReplayCodec::kCodecVersion},
      .temporaryRelativePath = std::filesystem::path("replay") / temporaryName,
  };
  return outcome;
}

ReplayInstallOutcome
ReplayFileStore::install(const ReplayFileReservation &reservation,
                         std::span<const std::byte> bytes) const {
  ReplayInstallOutcome outcome;
  if (!limits_.valid() || !canonicalIdentity(reservation.identity, limits_) ||
      !safeTemporaryRelativePath(reservation.temporaryRelativePath) ||
      reservation.attemptToken.empty() ||
      reservation.expectedMetadata.relativePath !=
          reservation.identity.relativePath ||
      reservation.expectedMetadata.sha256 != hashBytes(bytes) ||
      reservation.expectedMetadata.compressedSize != bytes.size() ||
      reservation.expectedMetadata.codecVersion !=
          BeatorajaReplayCodec::kCodecVersion ||
      bytes.empty() || bytes.size() > limits_.maxCompressedBytes) {
    outcome.state = ReplayInstallState::Unsafe;
    outcome.diagnostic = "Replay installation reservation is invalid";
    return outcome;
  }

  std::filesystem::path replayDirectory;
  if (!ensureReplayDirectory(profileRoot_, replayDirectory,
                             outcome.diagnostic)) {
    outcome.state = ReplayInstallState::Unsafe;
    return outcome;
  }
  const auto finalPath =
      profileRoot_ / reservation.expectedMetadata.relativePath;
  const auto temporaryPath = profileRoot_ / reservation.temporaryRelativePath;

  const auto existing =
      inspectAt(profileRoot_, reservation.expectedMetadata, limits_);
  if (existing.state == ReplayFileState::Available) {
    std::string cleanupDiagnostic;
    const auto tempKind = entryKind(temporaryPath, cleanupDiagnostic);
    if (tempKind == EntryKind::Regular || tempKind == EntryKind::Symlink ||
        tempKind == EntryKind::Other) {
      if (!removeEntryAndSync(temporaryPath, replayDirectory,
                              cleanupDiagnostic)) {
        outcome.state = ReplayInstallState::RetryableAmbiguous;
        outcome.diagnostic = std::move(cleanupDiagnostic);
        return outcome;
      }
    } else if (tempKind == EntryKind::Directory ||
               tempKind == EntryKind::Error) {
      outcome.state = ReplayInstallState::Unsafe;
      outcome.diagnostic = "Replay temporary path is unsafe";
      return outcome;
    }
    if (!atomic_file::syncDirectory(replayDirectory, outcome.diagnostic)) {
      outcome.state = ReplayInstallState::RetryableAmbiguous;
      return outcome;
    }
    outcome.file = installedFile(reservation, outcome.diagnostic);
    outcome.state = outcome.file ? ReplayInstallState::InstalledVerified
                                 : ReplayInstallState::Failed;
    outcome.existingIdenticalFile = outcome.file.has_value();
    return outcome;
  }
  if (existing.state == ReplayFileState::Corrupt) {
    outcome.state = ReplayInstallState::Occupied;
    outcome.diagnostic = "Replay destination is occupied by different bytes";
    return outcome;
  }
  if (existing.state == ReplayFileState::Unsafe) {
    outcome.state = ReplayInstallState::Unsafe;
    outcome.diagnostic = existing.diagnostic;
    return outcome;
  }
  if (existing.state == ReplayFileState::IoFailure) {
    outcome.state = ReplayInstallState::Failed;
    outcome.diagnostic = existing.diagnostic;
    return outcome;
  }

  std::string temporaryDiagnostic;
  const auto temporaryKind = entryKind(temporaryPath, temporaryDiagnostic);
  if (temporaryKind == EntryKind::Directory ||
      temporaryKind == EntryKind::Symlink ||
      temporaryKind == EntryKind::Other || temporaryKind == EntryKind::Error) {
    outcome.state = ReplayInstallState::Unsafe;
    outcome.diagnostic = "Replay temporary path is unsafe";
    return outcome;
  }
  if (fault(faults_, "temporary-write")) {
    outcome.state = ReplayInstallState::Failed;
    outcome.diagnostic = "Injected replay temporary-write failure";
    return outcome;
  }
  const auto operations = atomic_file::privateFileOperations();
  if (!operations.writeAndSync(temporaryPath, bytes, outcome.diagnostic)) {
    outcome.state = ReplayInstallState::Failed;
    return outcome;
  }
  if (fault(faults_, "install")) {
    std::string ignored;
    removeEntryAndSync(temporaryPath, replayDirectory, ignored);
    outcome.state = ReplayInstallState::Failed;
    outcome.diagnostic = "Injected replay install failure";
    return outcome;
  }

  std::error_code linkError;
  std::filesystem::create_hard_link(temporaryPath, finalPath, linkError);
  if (linkError) {
    const auto raced =
        inspectAt(profileRoot_, reservation.expectedMetadata, limits_);
    std::string ignored;
    removeEntryAndSync(temporaryPath, replayDirectory, ignored);
    if (raced.state == ReplayFileState::Available) {
      if (!atomic_file::syncDirectory(replayDirectory, outcome.diagnostic)) {
        outcome.state = ReplayInstallState::RetryableAmbiguous;
        return outcome;
      }
      outcome.file = installedFile(reservation, outcome.diagnostic);
      outcome.state = outcome.file ? ReplayInstallState::InstalledVerified
                                   : ReplayInstallState::Failed;
      outcome.existingIdenticalFile = outcome.file.has_value();
      return outcome;
    }
    outcome.state = raced.state == ReplayFileState::Corrupt
                        ? ReplayInstallState::Occupied
                        : ReplayInstallState::Failed;
    outcome.diagnostic = "Atomic replay install failed: " + linkError.message();
    return outcome;
  }
  if (fault(faults_, "after-install")) {
    outcome.state = ReplayInstallState::RetryableAmbiguous;
    outcome.diagnostic = "Injected interruption after replay install";
    return outcome;
  }

  std::error_code removeError;
  std::filesystem::remove(temporaryPath, removeError);
  if (removeError) {
    outcome.state = ReplayInstallState::RetryableAmbiguous;
    outcome.diagnostic =
        "Replay temporary cleanup is ambiguous: " + removeError.message();
    return outcome;
  }
  if (fault(faults_, "directory-sync")) {
    // The first acknowledgement is intentionally lost. Re-sync the parent
    // before treating the installed entry as durable.
    outcome.diagnostic.clear();
  }
  if (!atomic_file::syncDirectory(replayDirectory, outcome.diagnostic)) {
    outcome.state = ReplayInstallState::RetryableAmbiguous;
    return outcome;
  }
  if (fault(faults_, "installed-validation")) {
    outcome.state = ReplayInstallState::RetryableAmbiguous;
    outcome.diagnostic = "Injected replay validation interruption";
    return outcome;
  }
  const auto verified =
      inspectAt(profileRoot_, reservation.expectedMetadata, limits_);
  if (verified.state != ReplayFileState::Available) {
    outcome.state = ReplayInstallState::RetryableAmbiguous;
    outcome.diagnostic = verified.diagnostic;
    return outcome;
  }
  outcome.file = installedFile(reservation, outcome.diagnostic);
  outcome.state = outcome.file ? ReplayInstallState::InstalledVerified
                               : ReplayInstallState::Failed;
  return outcome;
}

ReplayFileInspection
ReplayFileStore::inspect(const ReplayFileMetadata &metadata) const {
  return inspectAt(profileRoot_, metadata, limits_);
}

ReplayFileReadOutcome
ReplayFileStore::readVerified(const ReplayFileMetadata &metadata) const {
  ReplayFileReadOutcome outcome;
  const auto inspection = inspect(metadata);
  outcome.state = inspection.state;
  outcome.diagnostic = inspection.diagnostic;
  if (inspection.state != ReplayFileState::Available) {
    return outcome;
  }
  const auto path = profileRoot_ / metadata.relativePath;
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    outcome.state = ReplayFileState::IoFailure;
    outcome.diagnostic = "Unable to open verified replay file";
    return outcome;
  }
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uint64_t>(end) != metadata.compressedSize ||
      static_cast<std::uint64_t>(end) > limits_.maxCompressedBytes) {
    outcome.state = ReplayFileState::Corrupt;
    outcome.diagnostic = "Replay size changed while opening verified bytes";
    return outcome;
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.seekg(0, std::ios::beg);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
      input.bad()) {
    outcome.state = ReplayFileState::IoFailure;
    outcome.diagnostic = "Unable to read complete verified replay bytes";
    return outcome;
  }
  if (hashBytes(bytes) != metadata.sha256) {
    outcome.state = ReplayFileState::Corrupt;
    outcome.diagnostic = "Replay hash changed while reading verified bytes";
    return outcome;
  }
  outcome.bytes = std::move(bytes);
  outcome.diagnostic.clear();
  return outcome;
}

bool ReplayFileStore::removeIfMatches(const ReplayFileMetadata &metadata,
                                      std::string &diagnostic) const {
  diagnostic.clear();
  const auto inspection = inspect(metadata);
  if (inspection.state == ReplayFileState::Missing) {
    return true;
  }
  if (inspection.state != ReplayFileState::Available) {
    diagnostic = inspection.diagnostic.empty()
                     ? "Replay bytes do not match expected ownership"
                     : inspection.diagnostic;
    return false;
  }
  return removeReferencedEntry(metadata, diagnostic);
}

bool ReplayFileStore::removeReferencedEntry(const ReplayFileMetadata &metadata,
                                            std::string &diagnostic) const {
  diagnostic.clear();
  if (!safeRelativePath(metadata.relativePath, limits_)) {
    diagnostic = "Replay metadata path is unsafe";
    return false;
  }
  std::filesystem::path replayDirectory;
  ReplayFileState absent = ReplayFileState::IoFailure;
  if (!existingReplayDirectory(profileRoot_, replayDirectory, absent,
                               diagnostic)) {
    return absent == ReplayFileState::Missing;
  }
  const auto path = profileRoot_ / metadata.relativePath;
  const auto kind = entryKind(path, diagnostic);
  if (kind == EntryKind::Missing) {
    return true;
  }
  if (kind == EntryKind::Directory || kind == EntryKind::Error) {
    if (diagnostic.empty()) {
      diagnostic = "Replay referenced entry is unsafe";
    }
    return false;
  }
  return removeEntryAndSync(path, replayDirectory, diagnostic);
}

void ReplayFileStore::removeStaleTemporaryFiles(
    std::chrono::system_clock::time_point cutoff) const {
  std::filesystem::path replayDirectory;
  ReplayFileState absent = ReplayFileState::IoFailure;
  std::string diagnostic;
  if (!existingReplayDirectory(profileRoot_, replayDirectory, absent,
                               diagnostic)) {
    return;
  }
  const auto fileCutoff = std::filesystem::file_time_type::clock::now() +
                          (cutoff - std::chrono::system_clock::now());
  bool removed = false;
  std::error_code iteratorError;
  for (std::filesystem::directory_iterator
           iterator(replayDirectory, iteratorError),
       end;
       !iteratorError && iterator != end; iterator.increment(iteratorError)) {
    const auto filename = iterator->path().filename().string();
    if (!isPrivateReplayTemporaryFilename(filename) ||
        fault(faults_, "cleanup")) {
      continue;
    }
    std::string kindDiagnostic;
    const auto kind = entryKind(iterator->path(), kindDiagnostic);
    if (kind == EntryKind::Directory || kind == EntryKind::Error ||
        kind == EntryKind::Missing) {
      continue;
    }
    std::error_code timeError;
    const auto modified =
        std::filesystem::last_write_time(iterator->path(), timeError);
    if (timeError || modified > fileCutoff) {
      continue;
    }
    std::error_code removeError;
    removed |=
        std::filesystem::remove(iterator->path(), removeError) && !removeError;
  }
  if (removed) {
    atomic_file::syncDirectory(replayDirectory, diagnostic);
  }
}

} // namespace replay
