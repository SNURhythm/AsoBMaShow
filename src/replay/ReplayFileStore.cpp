#include "ReplayFileStore.h"

#include "BeatorajaReplayCodec.h"
#include "ReplayFormat.h"
#include "../AtomicFile.h"
#include "../FileChecksum.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <ranges>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace replay {
namespace {

constexpr std::string_view kTemporaryPrefix = ".asobmashow-replay-";
constexpr std::size_t kAttemptDigestBytes = 64;
constexpr std::size_t kContentDigestPrefixBytes = 16;
constexpr std::string_view kTemporarySuffix = ".tmp";
constexpr std::string_view kRemovalDirectoryPrefix =
    ".asobmashow-replay-removal-";
constexpr std::string_view kCleanupLeaseFilename =
    ".asobmashow-replay-cleanup.lock";
constexpr std::string_view kShareDirectoryPrefix =
    "asobmashow-replay-share-";
constexpr std::size_t kShareDirectoryTokenBytes = 24;

std::mutex &cleanupThreadMutex() {
  static std::mutex mutex;
  return mutex;
}

class ReplayCleanupLease {
public:
  ReplayCleanupLease() = default;

  bool acquire(const std::filesystem::path &profileRoot,
               std::string &diagnostic) {
    threadLock_ =
        std::unique_lock<std::mutex>(cleanupThreadMutex(), std::try_to_lock);
    if (!threadLock_.owns_lock()) {
      diagnostic = "Replay cleanup is already in progress";
      return false;
    }

    const auto path = profileRoot / std::string(kCleanupLeaseFilename);
#if defined(_WIN32)
    handle_ = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      diagnostic = "Unable to open replay cleanup lease: " +
                   std::to_string(GetLastError());
      return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    BY_HANDLE_FILE_INFORMATION identity{};
    if (!GetFileInformationByHandleEx(handle_, FileAttributeTagInfo,
                                      &attributes, sizeof(attributes)) ||
        !GetFileInformationByHandle(handle_, &identity) ||
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        identity.nNumberOfLinks != 1) {
      diagnostic = "Replay cleanup lease is unsafe";
      return false;
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle_,
                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1,
                    0, &overlapped)) {
      diagnostic = GetLastError() == ERROR_LOCK_VIOLATION
                       ? "Replay cleanup is already in progress"
                       : "Unable to lock replay cleanup lease: " +
                             std::to_string(GetLastError());
      return false;
    }
    locked_ = true;
#else
    int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    descriptor_ = ::open(path.c_str(), flags, 0600);
    if (descriptor_ < 0) {
      diagnostic = "Unable to open replay cleanup lease: " +
                   std::string(std::strerror(errno));
      return false;
    }
    struct stat status{};
    if (::fstat(descriptor_, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || status.st_nlink != 1 ||
        ::fchmod(descriptor_, 0600) != 0 ||
        ::fstat(descriptor_, &status) != 0 || (status.st_mode & 0777) != 0600) {
      diagnostic = "Replay cleanup lease is unsafe";
      return false;
    }
    if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
      diagnostic = errno == EWOULDBLOCK || errno == EAGAIN
                       ? "Replay cleanup is already in progress"
                       : "Unable to lock replay cleanup lease: " +
                             std::string(std::strerror(errno));
      return false;
    }
    locked_ = true;
#endif
    return true;
  }

  ~ReplayCleanupLease() {
#if defined(_WIN32)
    if (locked_) {
      OVERLAPPED overlapped{};
      (void)UnlockFileEx(handle_, 0, 1, 0, &overlapped);
    }
    if (handle_ != INVALID_HANDLE_VALUE) {
      (void)CloseHandle(handle_);
    }
#else
    if (locked_) {
      (void)::flock(descriptor_, LOCK_UN);
    }
    if (descriptor_ >= 0) {
      (void)::close(descriptor_);
    }
#endif
  }

  ReplayCleanupLease(const ReplayCleanupLease &) = delete;
  ReplayCleanupLease &operator=(const ReplayCleanupLease &) = delete;

private:
  std::unique_lock<std::mutex> threadLock_;
  bool locked_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int descriptor_ = -1;
#endif
};

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

bool canonicalMetadata(const ReplayFileMetadata &metadata,
                       const ReplayLimits &limits, std::string &diagnostic) {
  if (isCanonicalReplayRelativePath(metadata.relativePath, diagnostic,
                                    limits) &&
      isCanonicalLowerHex(metadata.sha256, 64) && metadata.compressedSize > 0 &&
      metadata.compressedSize <= limits.maxCompressedBytes &&
      metadata.codecVersion > 0) {
    return true;
  }
  diagnostic = "Replay metadata is unsafe";
  return false;
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

ReplayFileInspection inspectFileAtPath(const std::filesystem::path &path,
                                       const ReplayFileMetadata &metadata,
                                       const ReplayLimits &limits);
ReplayFileInspection probeFileAtPath(const std::filesystem::path &path,
                                     const ReplayFileMetadata &metadata);
bool removeEntryAndSync(const std::filesystem::path &path,
                        const std::filesystem::path &directory,
                        std::string &diagnostic);

ReplayFileInspection inspectAt(const std::filesystem::path &profileRoot,
                               const ReplayFileMetadata &metadata,
                               const ReplayLimits &limits) {
  ReplayFileInspection outcome;
  if (!canonicalMetadata(metadata, limits, outcome.diagnostic)) {
    outcome.state = ReplayFileState::Unsafe;
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
  return inspectFileAtPath(path, metadata, limits);
}

ReplayFileInspection probeAt(const std::filesystem::path &profileRoot,
                             const ReplayFileMetadata &metadata,
                             const ReplayLimits &limits) {
  ReplayFileInspection outcome;
  if (!canonicalMetadata(metadata, limits, outcome.diagnostic)) {
    outcome.state = ReplayFileState::Unsafe;
    return outcome;
  }
  std::filesystem::path replayDirectory;
  ReplayFileState absentState = ReplayFileState::IoFailure;
  if (!existingReplayDirectory(profileRoot, replayDirectory, absentState,
                               outcome.diagnostic)) {
    outcome.state = absentState;
    return outcome;
  }
  return probeFileAtPath(profileRoot / metadata.relativePath, metadata);
}

ReplayFileInspection probeFileAtPath(const std::filesystem::path &path,
                                     const ReplayFileMetadata &metadata) {
  ReplayFileInspection outcome;
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
  outcome.state = ReplayFileState::Available;
  return outcome;
}

ReplayFileInspection inspectFileAtPath(const std::filesystem::path &path,
                                       const ReplayFileMetadata &metadata,
                                       const ReplayLimits &limits) {
  ReplayFileInspection outcome;
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
  const bool sizeMatches = size == metadata.compressedSize;
  std::string checksumError;
  const auto checksum =
      file_checksum::sha256File(path, checksumError, limits.maxCompressedBytes);
  if (!checksum) {
    outcome.state = sizeMatches ? ReplayFileState::IoFailure
                                : ReplayFileState::Corrupt;
    outcome.diagnostic = sizeMatches
                             ? std::move(checksumError)
                             : "Replay file size does not match metadata";
    return outcome;
  }
  if (size > 0) {
    outcome.observedMetadata = ReplayFileMetadata{
        .relativePath = metadata.relativePath,
        .compressedSize = size,
        .sha256 = *checksum,
        .codecVersion = metadata.codecVersion,
    };
  }
  if (!sizeMatches) {
    outcome.state = ReplayFileState::Corrupt;
    outcome.diagnostic = "Replay file size does not match metadata";
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

std::filesystem::path
removalDirectoryFor(const std::filesystem::path &replayDirectory,
                    const ReplayFileMetadata &metadata) {
  const std::string ownershipKey = metadata.relativePath.generic_string() +
                                   "\n" + metadata.sha256 + "\n" +
                                   std::to_string(metadata.compressedSize) +
                                   "\n" + std::to_string(metadata.codecVersion);
  return replayDirectory / (std::string(kRemovalDirectoryPrefix) +
                            file_checksum::sha256(ownershipKey));
}

bool removeEmptyDirectoryAndSync(const std::filesystem::path &directory,
                                 const std::filesystem::path &parent,
                                 std::string &diagnostic) {
  const auto kind = entryKind(directory, diagnostic);
  if (kind == EntryKind::Missing) {
    return true;
  }
  if (kind != EntryKind::Directory) {
    if (diagnostic.empty()) {
      diagnostic = "Replay cleanup directory is unsafe";
    }
    return false;
  }
  std::error_code error;
  const bool removed = std::filesystem::remove(directory, error);
  if (error) {
    diagnostic =
        "Unable to remove replay cleanup directory: " + error.message();
    return false;
  }
  if (!removed) {
    diagnostic = "Replay cleanup directory contains unexpected entries";
    return false;
  }
  return atomic_file::syncDirectory(parent, diagnostic);
}

bool restoreQuarantinedRegularFile(
    const std::filesystem::path &quarantined,
    const std::filesystem::path &original,
    const std::filesystem::path &quarantineDirectory,
    const std::filesystem::path &replayDirectory, std::string &diagnostic) {
  std::error_code linkError;
  std::filesystem::create_hard_link(quarantined, original, linkError);
  if (linkError) {
    diagnostic += diagnostic.empty() ? "" : "; ";
    diagnostic += "Unmatched replay bytes were preserved in private cleanup "
                  "quarantine '" +
                  quarantined.string() + "': " + linkError.message();
    return false;
  }
  if (!atomic_file::syncDirectory(replayDirectory, diagnostic) ||
      !removeEntryAndSync(quarantined, quarantineDirectory, diagnostic) ||
      !removeEmptyDirectoryAndSync(quarantineDirectory, replayDirectory,
                                   diagnostic)) {
    return false;
  }
  return true;
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

class ReplayFileSnapshotLifetime {
public:
  explicit ReplayFileSnapshotLifetime(std::filesystem::path directory)
      : directory_(std::move(directory)) {}
  ~ReplayFileSnapshotLifetime() {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

private:
  std::filesystem::path directory_;
};

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
  return isCanonicalLowerHex(attempt, kAttemptDigestBytes) &&
         isCanonicalLowerHex(content, kContentDigestPrefixBytes) &&
         filename == kTemporarySuffix;
}

bool isPrivateReplayShareDirectoryName(std::string_view filename) noexcept {
  if (filename.size() !=
          kShareDirectoryPrefix.size() + kShareDirectoryTokenBytes ||
      !filename.starts_with(kShareDirectoryPrefix)) {
    return false;
  }
  filename.remove_prefix(kShareDirectoryPrefix.size());
  return isCanonicalLowerHex(filename, kShareDirectoryTokenBytes);
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
                         std::span<const std::byte> bytes,
                         const ReplayInstallOwnershipJournal &ownershipJournal)
    const {
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

  if (ownershipJournal) {
    const ReplayFileOwnershipReceipt receipt{
        .attemptToken = reservation.attemptToken,
        .metadata = reservation.expectedMetadata,
    };
    if (!ownershipJournal(receipt, outcome.diagnostic)) {
      std::string cleanupDiagnostic;
      if (!removeEntryAndSync(temporaryPath, replayDirectory,
                              cleanupDiagnostic) &&
          outcome.diagnostic.empty()) {
        outcome.diagnostic = std::move(cleanupDiagnostic);
      }
      if (outcome.diagnostic.empty()) {
        outcome.diagnostic = "Replay install ownership journal failed";
      }
      outcome.state = ReplayInstallState::Failed;
      return outcome;
    }
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

ReplayFileInspection
ReplayFileStore::probe(const ReplayFileMetadata &metadata) const {
  return probeAt(profileRoot_, metadata, limits_);
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

ReplayFileSnapshotOutcome ReplayFileStore::stageVerifiedSnapshot(
    const ReplayFileMetadata &metadata) const {
  ReplayFileSnapshotOutcome outcome;
  const auto read = readVerified(metadata);
  outcome.state = read.state;
  outcome.diagnostic = read.diagnostic;
  if (read.state != ReplayFileState::Available || !read.bytes) {
    return outcome;
  }
  if (fault(faults_, "share-snapshot")) {
    outcome.state = ReplayFileState::IoFailure;
    outcome.diagnostic = "Injected replay share snapshot failure";
    return outcome;
  }

  const auto temporaryRoot = std::filesystem::temp_directory_path();
  std::filesystem::path directory;
  bool created = false;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const std::string token = file_checksum::sha256(
        metadata.sha256 + ":" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count()) +
        ":" + std::to_string(attempt));
    directory = temporaryRoot /
                (std::string(kShareDirectoryPrefix) +
                 token.substr(0, kShareDirectoryTokenBytes));
    std::error_code error;
    created = std::filesystem::create_directory(directory, error);
    if (created) {
      break;
    }
    if (error && error != std::errc::file_exists) {
      outcome.state = ReplayFileState::IoFailure;
      outcome.diagnostic =
          "Unable to create replay share staging: " + error.message();
      return outcome;
    }
  }
  if (!created) {
    outcome.state = ReplayFileState::IoFailure;
    outcome.diagnostic = "Unable to reserve replay share staging";
    return outcome;
  }

  const auto snapshot = directory / metadata.relativePath.filename();
  const auto operations = atomic_file::privateFileOperations();
  if (!operations.writeAndSync(snapshot, *read.bytes, outcome.diagnostic) ||
      !atomic_file::syncDirectory(directory, outcome.diagnostic)) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    outcome.state = ReplayFileState::IoFailure;
    return outcome;
  }
  ReplayFileMetadata stagedMetadata = metadata;
  stagedMetadata.relativePath = metadata.relativePath;
  std::string checksumError;
  const auto checksum = file_checksum::sha256File(
      snapshot, checksumError, limits_.maxCompressedBytes);
  std::error_code sizeError;
  const auto size = std::filesystem::file_size(snapshot, sizeError);
  if (!checksum || *checksum != metadata.sha256 || sizeError ||
      size != metadata.compressedSize) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    outcome.state = ReplayFileState::IoFailure;
    outcome.diagnostic = checksumError.empty()
                             ? "Replay share snapshot verification failed"
                             : std::move(checksumError);
    return outcome;
  }
  outcome.state = ReplayFileState::Available;
  outcome.snapshot = ReplayFileSnapshot{
      .sourcePath = snapshot,
      .compressedSize = metadata.compressedSize,
      .sourceLifetime =
          std::make_shared<ReplayFileSnapshotLifetime>(directory)};
  outcome.diagnostic.clear();
  return outcome;
}

bool ReplayFileStore::removeIfMatches(const ReplayFileMetadata &metadata,
                                      std::string &diagnostic) const {
  diagnostic.clear();
  if (!canonicalMetadata(metadata, limits_, diagnostic)) {
    return false;
  }

  std::filesystem::path replayDirectory;
  ReplayFileState absent = ReplayFileState::IoFailure;
  if (!existingReplayDirectory(profileRoot_, replayDirectory, absent,
                               diagnostic)) {
    return absent == ReplayFileState::Missing;
  }
  ReplayCleanupLease cleanupLease;
  if (!cleanupLease.acquire(profileRoot_, diagnostic)) {
    return false;
  }
  const auto finalPath = profileRoot_ / metadata.relativePath;
  const auto quarantineDirectory =
      removalDirectoryFor(replayDirectory, metadata);
  const auto quarantinePath = quarantineDirectory / "owned.brd";

  auto quarantineKind = entryKind(quarantineDirectory, diagnostic);
  if (quarantineKind == EntryKind::Missing) {
    std::error_code createError;
    if (!std::filesystem::create_directory(quarantineDirectory, createError) ||
        createError) {
      diagnostic = "Unable to reserve replay cleanup quarantine: " +
                   createError.message();
      return false;
    }
    std::error_code permissionError;
#ifndef _WIN32
    std::filesystem::permissions(
        quarantineDirectory, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, permissionError);
#endif
    if (permissionError ||
        !atomic_file::syncDirectory(replayDirectory, diagnostic)) {
      if (diagnostic.empty()) {
        diagnostic = "Unable to protect replay cleanup quarantine: " +
                     permissionError.message();
      }
      return false;
    }
    quarantineKind = EntryKind::Directory;
  }
  if (quarantineKind != EntryKind::Directory) {
    if (diagnostic.empty()) {
      diagnostic = "Replay cleanup quarantine is unsafe";
    }
    return false;
  }

  auto quarantinedKind = entryKind(quarantinePath, diagnostic);
  if (quarantinedKind == EntryKind::Missing) {
    const auto finalKind = entryKind(finalPath, diagnostic);
    if (finalKind == EntryKind::Missing) {
      return removeEmptyDirectoryAndSync(quarantineDirectory, replayDirectory,
                                         diagnostic);
    }
    if (finalKind != EntryKind::Regular) {
      if (diagnostic.empty()) {
        diagnostic = "Replay cleanup target is unsafe";
      }
      return false;
    }
    if (fault(faults_, "remove-before-quarantine")) {
      diagnostic = "Injected replay cleanup interruption before quarantine";
      return false;
    }
    if (!atomic_file::renameDurably(finalPath, quarantinePath, diagnostic) ||
        !atomic_file::syncDirectory(replayDirectory, diagnostic) ||
        !atomic_file::syncDirectory(quarantineDirectory, diagnostic)) {
      return false;
    }
    quarantinedKind = entryKind(quarantinePath, diagnostic);
  }
  if (quarantinedKind != EntryKind::Regular) {
    if (diagnostic.empty()) {
      diagnostic = "Quarantined replay cleanup target is unsafe";
    }
    return false;
  }
  if (fault(faults_, "remove-after-quarantine")) {
    diagnostic = "Injected replay cleanup interruption after quarantine";
    return false;
  }

  const auto inspection = inspectFileAtPath(quarantinePath, metadata, limits_);
  if (inspection.state != ReplayFileState::Available) {
    const std::string mismatchDiagnostic =
        inspection.diagnostic.empty()
            ? "Replay bytes do not match expected ownership"
            : inspection.diagnostic;
    std::string restoreDiagnostic;
    const bool restored = restoreQuarantinedRegularFile(
        quarantinePath, finalPath, quarantineDirectory, replayDirectory,
        restoreDiagnostic);
    diagnostic = mismatchDiagnostic;
    if (!restored && !restoreDiagnostic.empty()) {
      diagnostic += "; " + restoreDiagnostic;
    }
    return false;
  }
  return removeEntryAndSync(quarantinePath, quarantineDirectory, diagnostic) &&
         removeEmptyDirectoryAndSync(quarantineDirectory, replayDirectory,
                                     diagnostic);
}

bool ReplayFileStore::removeReferencedEntry(const ReplayFileMetadata &metadata,
                                            std::string &diagnostic) const {
  diagnostic.clear();
  if (!isCanonicalReplayRelativePath(metadata.relativePath, diagnostic,
                                     limits_)) {
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

void ReplayFileStore::removeStaleShareSnapshots(
    std::chrono::system_clock::time_point cutoff) const {
  const auto temporaryRoot = std::filesystem::temp_directory_path();
  const auto fileCutoff = std::filesystem::file_time_type::clock::now() +
                          (cutoff - std::chrono::system_clock::now());
  std::error_code iteratorError;
  for (std::filesystem::directory_iterator iterator(temporaryRoot,
                                                     iteratorError),
       end;
       !iteratorError && iterator != end; iterator.increment(iteratorError)) {
    const auto directory = iterator->path();
    if (!isPrivateReplayShareDirectoryName(
            directory.filename().string()) ||
        fault(faults_, "cleanup")) {
      continue;
    }
    std::string kindDiagnostic;
    if (entryKind(directory, kindDiagnostic) != EntryKind::Directory) {
      continue;
    }
    std::error_code timeError;
    const auto modified = std::filesystem::last_write_time(directory, timeError);
    if (timeError || modified > fileCutoff) {
      continue;
    }

    std::filesystem::path snapshot;
    std::size_t entries = 0;
    std::error_code childError;
    for (std::filesystem::directory_iterator child(directory, childError),
         childEnd;
         !childError && child != childEnd; child.increment(childError)) {
      snapshot = child->path();
      ++entries;
      if (entries > 1) {
        break;
      }
    }
    if (childError || entries != 1 ||
        entryKind(snapshot, kindDiagnostic) != EntryKind::Regular) {
      continue;
    }
    std::string pathDiagnostic;
    if (!isCanonicalReplayRelativePath(
            std::filesystem::path("replay") / snapshot.filename(),
            pathDiagnostic, limits_)) {
      continue;
    }

    std::error_code removeError;
    if (!std::filesystem::remove(snapshot, removeError) || removeError) {
      continue;
    }
    removeError.clear();
    (void)std::filesystem::remove(directory, removeError);
  }
}

} // namespace replay
