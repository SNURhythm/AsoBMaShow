#include "ReplayFileStore.h"

#include "../AtomicFile.h"
#include "../FileChecksum.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace replay {
namespace {

enum class ReadState { Success, Missing, SizeExceeded, Unsafe, IoFailure };

struct ReadOutcome {
  ReadState state = ReadState::IoFailure;
  std::vector<std::byte> bytes;
  std::string diagnostic;
};

bool canonicalLowerHex(std::string_view value, std::size_t length) {
  return value.size() == length &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool failAt(const ReplayFileStoreFaults &faults, std::string_view point) {
  return faults.failAt && faults.failAt(point);
}

std::filesystem::path normalizedAbsolute(const std::filesystem::path &path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal();
}

bool safeRelativeReplayPath(const std::filesystem::path &relative) {
  if (relative.empty() || relative.is_absolute() ||
      relative != relative.lexically_normal() ||
      relative.parent_path() != std::filesystem::path("replay") ||
      relative.filename().empty() || relative.extension() != ".brd") {
    return false;
  }
  std::size_t components = 0;
  for (const auto &component : relative) {
    if (component == "." || component == ".." || component.empty()) {
      return false;
    }
    ++components;
  }
  return components == 2;
}

bool safeAttemptToken(std::string_view token) {
  return !token.empty() && token.size() <= 128 &&
         std::ranges::all_of(token, [](unsigned char ch) {
           return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
         });
}

bool entryMissing(const std::error_code &error) {
  return error == std::errc::no_such_file_or_directory;
}

bool ensureReplayDirectory(const std::filesystem::path &profileRoot,
                           std::filesystem::path &replayDirectory,
                           std::string &diagnostic) {
  diagnostic.clear();
  std::error_code error;
  const auto rootStatus = std::filesystem::symlink_status(profileRoot, error);
  if (error || !std::filesystem::is_directory(rootStatus) ||
      std::filesystem::is_symlink(rootStatus)) {
    diagnostic = "Replay profile root is missing, linked, or not a directory";
    return false;
  }

  replayDirectory = profileRoot / "replay";
  auto replayStatus = std::filesystem::symlink_status(replayDirectory, error);
  if (entryMissing(error) ||
      replayStatus.type() == std::filesystem::file_type::not_found) {
    error.clear();
    if (!std::filesystem::create_directory(replayDirectory, error) || error) {
      diagnostic =
          "Could not create profile replay directory: " + error.message();
      return false;
    }
    if (!atomic_file::syncDirectory(profileRoot, diagnostic)) {
      return false;
    }
    replayStatus = std::filesystem::symlink_status(replayDirectory, error);
  }
  if (error || !std::filesystem::is_directory(replayStatus) ||
      std::filesystem::is_symlink(replayStatus)) {
    diagnostic = "Profile replay path is linked or not a real directory";
    return false;
  }

  const auto canonicalRoot = std::filesystem::canonical(profileRoot, error);
  if (error) {
    diagnostic =
        "Could not canonicalize replay profile root: " + error.message();
    return false;
  }
  const auto canonicalReplay =
      std::filesystem::canonical(replayDirectory, error);
  if (error || canonicalReplay != canonicalRoot / "replay") {
    diagnostic = "Profile replay directory escapes its canonical root";
    return false;
  }
  return true;
}

bool resolveContained(const std::filesystem::path &profileRoot,
                      const std::filesystem::path &relative,
                      std::filesystem::path &resolved,
                      std::string &diagnostic) {
  if (!safeRelativeReplayPath(relative)) {
    diagnostic = "Replay path is absolute, traversing, or non-canonical";
    return false;
  }
  std::filesystem::path replayDirectory;
  if (!ensureReplayDirectory(profileRoot, replayDirectory, diagnostic)) {
    return false;
  }
  resolved = (profileRoot / relative).lexically_normal();
  if (resolved.parent_path() != replayDirectory) {
    diagnostic =
        "Replay path does not resolve inside the profile replay directory";
    return false;
  }
  return true;
}

ReadOutcome readRegularNoLinks(const std::filesystem::path &path,
                               std::uint64_t maximumBytes) {
  ReadOutcome outcome;
#ifdef _WIN32
  HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      outcome.state = ReadState::Missing;
    } else {
      outcome.diagnostic =
          "Could not open replay file: " + std::to_string(error);
    }
    return outcome;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info)) {
    outcome.diagnostic = "Could not inspect replay file handle";
    CloseHandle(handle);
    return outcome;
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      info.nNumberOfLinks != 1) {
    outcome.state = ReadState::Unsafe;
    outcome.diagnostic = "Replay file is linked or not a private regular file";
    CloseHandle(handle);
    return outcome;
  }
  const std::uint64_t size =
      (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) |
      info.nFileSizeLow;
  if (size > maximumBytes ||
      size >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    outcome.state = ReadState::SizeExceeded;
    outcome.diagnostic = "Replay file exceeds its expected size bound";
    CloseHandle(handle);
    return outcome;
  }
  outcome.bytes.resize(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < outcome.bytes.size()) {
    const DWORD requested = static_cast<DWORD>(
        std::min<std::size_t>(outcome.bytes.size() - offset, MAXDWORD));
    DWORD count = 0;
    if (!ReadFile(handle, outcome.bytes.data() + offset, requested, &count,
                  nullptr) ||
        count == 0) {
      outcome.diagnostic = "Could not read complete replay file";
      CloseHandle(handle);
      return outcome;
    }
    offset += count;
  }
  CloseHandle(handle);
#else
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int descriptor = ::open(path.c_str(), flags);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      outcome.state = ReadState::Missing;
    } else if (errno == ELOOP) {
      outcome.state = ReadState::Unsafe;
      outcome.diagnostic = "Replay file is a symbolic link";
    } else {
      outcome.diagnostic =
          "Could not open replay file: " + std::string(std::strerror(errno));
    }
    return outcome;
  }
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    outcome.diagnostic =
        "Could not inspect replay file: " + std::string(std::strerror(errno));
    ::close(descriptor);
    return outcome;
  }
  if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
    outcome.state = ReadState::Unsafe;
    outcome.diagnostic = "Replay file is not a private regular file";
    ::close(descriptor);
    return outcome;
  }
  if (status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > maximumBytes ||
      static_cast<std::uint64_t>(status.st_size) >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    outcome.state = ReadState::SizeExceeded;
    outcome.diagnostic = "Replay file exceeds its expected size bound";
    ::close(descriptor);
    return outcome;
  }
  outcome.bytes.resize(static_cast<std::size_t>(status.st_size));
  std::size_t offset = 0;
  while (offset < outcome.bytes.size()) {
    const ssize_t count = ::read(descriptor, outcome.bytes.data() + offset,
                                 outcome.bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      outcome.diagnostic = "Could not read complete replay file";
      ::close(descriptor);
      return outcome;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (::close(descriptor) != 0) {
    outcome.diagnostic =
        "Could not close replay file: " + std::string(std::strerror(errno));
    return outcome;
  }
#endif
  outcome.state = ReadState::Success;
  return outcome;
}

bool privateRegularPath(const std::filesystem::path &path, bool &missing,
                        std::string &diagnostic) {
  missing = false;
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (entryMissing(error) ||
      status.type() == std::filesystem::file_type::not_found) {
    missing = true;
    diagnostic.clear();
    return true;
  }
  if (error) {
    diagnostic = "Could not inspect replay deletion target: " + error.message();
    return false;
  }
  if (!std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    diagnostic = "Replay deletion target is linked or not a regular file";
    return false;
  }
  const auto links = std::filesystem::hard_link_count(path, error);
  if (error || links != 1) {
    diagnostic = "Replay deletion target has an unsafe hard-link count";
    return false;
  }
  return true;
}

std::string bytesHash(std::span<const std::byte> bytes) {
  file_checksum::Sha256 hash;
  hash.update(bytes);
  return hash.finalHex();
}

bool expectedIdentityValid(const ExpectedReplayIdentity &expected) {
  if (expected.stageSha256.empty() ||
      (!expected.course && expected.stageSha256.size() != 1)) {
    return false;
  }
  return std::ranges::all_of(expected.stageSha256, [](const std::string &hash) {
    return canonicalLowerHex(hash, 64);
  });
}

bool decodedIdentityMatches(const ReplayDecodeOutcome &decoded,
                            const ExpectedReplayIdentity &expected) {
  if (!expected.course) {
    return decoded.chart && !decoded.course &&
           decoded.chart->setup.chartSha256 == expected.stageSha256.front();
  }
  if (!decoded.course || decoded.chart ||
      decoded.course->stages.size() != expected.stageSha256.size()) {
    return false;
  }
  for (std::size_t i = 0; i < expected.stageSha256.size(); ++i) {
    if (decoded.course->stages[i].setup.chartSha256 !=
        expected.stageSha256[i]) {
      return false;
    }
  }
  return true;
}

std::filesystem::path temporaryPathFor(const std::filesystem::path &finalPath,
                                       std::string_view attemptToken) {
  return finalPath.parent_path() / ("." + finalPath.filename().string() + "." +
                                    std::string(attemptToken) + ".tmp");
}

void removePrivateTemporary(const std::filesystem::path &temporary) {
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);
  std::filesystem::remove(temporary.string() + ".tmp", ignored);
  std::filesystem::remove(temporary.string() + ".bak", ignored);
  std::filesystem::remove(temporary.string() + ".bak.pending", ignored);
  std::filesystem::remove(temporary.string() + ".bak.previous", ignored);
}

FinalizeOutcome validateFinal(const std::filesystem::path &finalPath,
                              const std::filesystem::path &relativePath,
                              std::span<const std::byte> expectedBytes,
                              const BeatorajaReplayCodec &codec,
                              const ExpectedReplayIdentity &expected,
                              const ReplayFileStoreFaults &faults,
                              bool existing) {
  FinalizeOutcome outcome;
  if (failAt(faults, "read-back")) {
    outcome.diagnostic = "Injected replay read-back failure";
    return outcome;
  }
  const auto read = readRegularNoLinks(finalPath, expectedBytes.size());
  if (read.state != ReadState::Success) {
    outcome.diagnostic = read.diagnostic.empty()
                             ? "Final replay could not be read safely"
                             : read.diagnostic;
    return outcome;
  }
  if (read.bytes.size() != expectedBytes.size() ||
      !std::equal(read.bytes.begin(), read.bytes.end(),
                  expectedBytes.begin())) {
    outcome.diagnostic =
        "Existing replay path contains different or oversized bytes";
    return outcome;
  }

  if (failAt(faults, "decode")) {
    outcome.diagnostic = "Injected replay decode failure";
    return outcome;
  }
  const auto decoded = codec.decode(read.bytes);
  if ((!decoded.chart && !decoded.course) ||
      !decodedIdentityMatches(decoded, expected)) {
    outcome.diagnostic = decoded.diagnostic.empty()
                             ? "Decoded replay identity does not match its path"
                             : decoded.diagnostic;
    return outcome;
  }

  if (failAt(faults, "hash")) {
    outcome.diagnostic = "Injected replay checksum failure";
    return outcome;
  }
  std::string hashDiagnostic;
  const auto hash = file_checksum::sha256File(
      finalPath, hashDiagnostic, static_cast<std::uint64_t>(read.bytes.size()));
  if (!hash) {
    outcome.diagnostic = hashDiagnostic;
    return outcome;
  }
  if (*hash != bytesHash(read.bytes)) {
    outcome.diagnostic = "Replay checksum changed during final verification";
    return outcome;
  }
  outcome.metadata = ReplayFileMetadata{
      .relativePath = relativePath,
      .sha256 = *hash,
      .compressedSize = static_cast<std::uint64_t>(read.bytes.size()),
      .codecVersion = BeatorajaReplayCodec::kCodecVersion,
  };
  outcome.existingIdenticalFile = existing;
  return outcome;
}

} // namespace

ReplayFileStore::ReplayFileStore(std::filesystem::path profileRoot,
                                 ReplayFileStoreFaults faults)
    : profileRoot_(normalizedAbsolute(profileRoot)),
      faults_(std::move(faults)) {}

FinalizeOutcome ReplayFileStore::finalize(
    const ReplayPathIdentity &identity, std::span<const std::byte> encoded,
    const BeatorajaReplayCodec &codec, const ExpectedReplayIdentity &expected,
    std::string_view attemptToken) {
  FinalizeOutcome outcome;
  if (!safeAttemptToken(attemptToken)) {
    outcome.diagnostic = "Replay attempt token is empty or unsafe";
    return outcome;
  }
  if (!expectedIdentityValid(expected)) {
    outcome.diagnostic = "Expected replay identity is invalid";
    return outcome;
  }
  std::filesystem::path finalPath;
  if (!resolveContained(profileRoot_, identity.relativePath, finalPath,
                        outcome.diagnostic)) {
    return outcome;
  }

  std::error_code statusError;
  const auto existingStatus =
      std::filesystem::symlink_status(finalPath, statusError);
  if (!statusError &&
      existingStatus.type() != std::filesystem::file_type::not_found) {
    return validateFinal(finalPath, identity.relativePath, encoded, codec,
                         expected, faults_, true);
  }
  if (statusError && !entryMissing(statusError)) {
    outcome.diagnostic =
        "Could not inspect final replay path: " + statusError.message();
    return outcome;
  }

  const auto temporary = temporaryPathFor(finalPath, attemptToken);
  removePrivateTemporary(temporary);
  if (failAt(faults_, "write")) {
    outcome.diagnostic = "Injected replay write failure";
    return outcome;
  }
  const auto privateOperations = atomic_file::privateFileOperations();
  if (!atomic_file::writeWithoutBackup(temporary, encoded, outcome.diagnostic,
                                       &privateOperations)) {
    removePrivateTemporary(temporary);
    return outcome;
  }
  if (failAt(faults_, "file-sync")) {
    outcome.diagnostic = "Injected replay file-sync failure";
    removePrivateTemporary(temporary);
    return outcome;
  }
  if (failAt(faults_, "close")) {
    outcome.diagnostic = "Injected replay close failure";
    removePrivateTemporary(temporary);
    return outcome;
  }
  if (failAt(faults_, "rename")) {
    outcome.diagnostic = "Injected replay no-replace rename failure";
    removePrivateTemporary(temporary);
    return outcome;
  }

  const auto rename = atomic_file::renameNoReplaceDurably(temporary, finalPath,
                                                          outcome.diagnostic);
  if (rename == atomic_file::RenameNoReplaceResult::DestinationExists) {
    removePrivateTemporary(temporary);
    return validateFinal(finalPath, identity.relativePath, encoded, codec,
                         expected, faults_, true);
  }
  if (rename != atomic_file::RenameNoReplaceResult::Renamed) {
    return outcome;
  }
  if (failAt(faults_, "directory-sync")) {
    outcome.diagnostic = "Injected replay directory-sync failure";
    return outcome;
  }
  if (!atomic_file::syncDirectory(finalPath.parent_path(),
                                  outcome.diagnostic)) {
    return outcome;
  }
  return validateFinal(finalPath, identity.relativePath, encoded, codec,
                       expected, faults_, false);
}

ReplayFileInspection
ReplayFileStore::inspect(const ReplayFileMetadata &metadata) const {
  ReplayFileInspection inspection;
  if (!canonicalLowerHex(metadata.sha256, 64) ||
      metadata.codecVersion != BeatorajaReplayCodec::kCodecVersion) {
    inspection.state = ReplayFileState::Corrupt;
    inspection.diagnostic =
        "Replay metadata checksum or codec version is invalid";
    return inspection;
  }
  std::filesystem::path path;
  if (!resolveContained(profileRoot_, metadata.relativePath, path,
                        inspection.diagnostic)) {
    inspection.state = ReplayFileState::Unsafe;
    return inspection;
  }
  const auto read = readRegularNoLinks(path, metadata.compressedSize);
  if (read.state == ReadState::Missing) {
    inspection.state = ReplayFileState::Missing;
    inspection.diagnostic = "Replay file is missing";
    return inspection;
  }
  if (read.state == ReadState::Unsafe) {
    inspection.state = ReplayFileState::Unsafe;
    inspection.diagnostic = read.diagnostic;
    return inspection;
  }
  if (read.state == ReadState::SizeExceeded) {
    inspection.state = ReplayFileState::Corrupt;
    inspection.diagnostic = read.diagnostic;
    return inspection;
  }
  if (read.state != ReadState::Success) {
    inspection.state = ReplayFileState::IoFailure;
    inspection.diagnostic = read.diagnostic;
    return inspection;
  }
  if (read.bytes.size() != metadata.compressedSize) {
    inspection.state = ReplayFileState::Corrupt;
    inspection.diagnostic = "Replay file size does not match metadata";
    return inspection;
  }
  if (failAt(faults_, "hash")) {
    inspection.state = ReplayFileState::IoFailure;
    inspection.diagnostic = "Injected replay checksum failure";
    return inspection;
  }
  std::string diagnostic;
  const auto hash =
      file_checksum::sha256File(path, diagnostic, metadata.compressedSize);
  if (!hash) {
    inspection.state = ReplayFileState::IoFailure;
    inspection.diagnostic = std::move(diagnostic);
    return inspection;
  }
  if (*hash != metadata.sha256) {
    inspection.state = ReplayFileState::Corrupt;
    inspection.diagnostic = "Replay file checksum does not match metadata";
    return inspection;
  }
  inspection.state = ReplayFileState::Available;
  inspection.metadata = metadata;
  return inspection;
}

ReplayDecodeOutcome
ReplayFileStore::load(const ReplayFileMetadata &metadata,
                      const BeatorajaReplayCodec &codec) const {
  ReplayDecodeOutcome outcome;
  const auto inspection = inspect(metadata);
  if (inspection.state != ReplayFileState::Available) {
    outcome.diagnostic = inspection.diagnostic;
    return outcome;
  }
  std::filesystem::path path;
  if (!resolveContained(profileRoot_, metadata.relativePath, path,
                        outcome.diagnostic)) {
    return outcome;
  }
  const auto read = readRegularNoLinks(path, metadata.compressedSize);
  if (read.state != ReadState::Success ||
      read.bytes.size() != metadata.compressedSize) {
    outcome.diagnostic = read.diagnostic.empty()
                             ? "Replay changed after inspection"
                             : read.diagnostic;
    return outcome;
  }
  if (bytesHash(read.bytes) != metadata.sha256) {
    outcome.diagnostic = "Replay changed after checksum inspection";
    return outcome;
  }
  return codec.decode(read.bytes);
}

bool ReplayFileStore::remove(const ReplayFileMetadata &metadata,
                             std::string &diagnostic) {
  std::filesystem::path path;
  if (!resolveContained(profileRoot_, metadata.relativePath, path,
                        diagnostic)) {
    return false;
  }
  bool missing = false;
  if (!privateRegularPath(path, missing, diagnostic)) {
    return false;
  }
  if (missing) {
    return true;
  }
  std::error_code error;
  if (!std::filesystem::remove(path, error) || error) {
    diagnostic = "Could not remove replay file: " + error.message();
    return false;
  }
  return atomic_file::syncDirectory(path.parent_path(), diagnostic);
}

bool ReplayFileStore::copyToBeatorajaSlot(const ReplayFileMetadata &source,
                                          std::string_view stem, int slot,
                                          std::string &diagnostic) {
  diagnostic.clear();
  if (slot < 0 || slot > 3) {
    diagnostic = "Beatoraja visible replay slot must be between 0 and 3";
    return false;
  }
  const auto sourceInspection = inspect(source);
  if (sourceInspection.state != ReplayFileState::Available) {
    diagnostic = sourceInspection.diagnostic;
    return false;
  }
  std::filesystem::path sourcePath;
  if (!resolveContained(profileRoot_, source.relativePath, sourcePath,
                        diagnostic)) {
    return false;
  }
  const auto sourceRead = readRegularNoLinks(sourcePath, source.compressedSize);
  if (sourceRead.state != ReadState::Success ||
      sourceRead.bytes.size() != source.compressedSize ||
      bytesHash(sourceRead.bytes) != source.sha256) {
    diagnostic = "Replay source changed before slot copy";
    return false;
  }

  std::string pathDiagnostic;
  const auto identity = pathForStem(stem, slot, pathDiagnostic);
  if (!identity) {
    diagnostic = std::move(pathDiagnostic);
    return false;
  }
  std::filesystem::path destination;
  if (!resolveContained(profileRoot_, identity->relativePath, destination,
                        diagnostic)) {
    return false;
  }
  if (destination == sourcePath) {
    return true;
  }

  bool destinationMissing = false;
  if (!privateRegularPath(destination, destinationMissing, diagnostic)) {
    return false;
  }
  if (!destinationMissing) {
    const auto existing =
        readRegularNoLinks(destination, source.compressedSize);
    if (existing.state == ReadState::Success &&
        existing.bytes == sourceRead.bytes) {
      return true;
    }
    if (existing.state != ReadState::Success &&
        existing.state != ReadState::SizeExceeded &&
        existing.state != ReadState::Missing) {
      diagnostic = existing.diagnostic.empty()
                       ? "Beatoraja replay slot is unsafe"
                       : existing.diagnostic;
      return false;
    }
  }

  const auto temporary = temporaryPathFor(destination, "slot_copy");
  removePrivateTemporary(temporary);
  const auto privateOperations = atomic_file::privateFileOperations();
  if (!atomic_file::writeWithoutBackup(temporary, sourceRead.bytes, diagnostic,
                                       &privateOperations)) {
    removePrivateTemporary(temporary);
    return false;
  }
  if (!privateRegularPath(destination, destinationMissing, diagnostic)) {
    removePrivateTemporary(temporary);
    return false;
  }
  if (!atomic_file::renameDurably(temporary, destination, diagnostic)) {
    removePrivateTemporary(temporary);
    return false;
  }
  return atomic_file::syncDirectory(destination.parent_path(), diagnostic);
}

void ReplayFileStore::removeStaleTemporaryFiles(
    std::chrono::system_clock::time_point cutoff) {
  std::filesystem::path replayDirectory;
  std::string diagnostic;
  if (!ensureReplayDirectory(profileRoot_, replayDirectory, diagnostic)) {
    return;
  }
  std::error_code error;
  bool removedAny = false;
  for (std::filesystem::directory_iterator iterator(replayDirectory, error),
       end;
       !error && iterator != end; iterator.increment(error)) {
    const std::string filename = iterator->path().filename().string();
    if (filename.size() < 8 || filename.front() != '.' ||
        !filename.ends_with(".tmp") ||
        filename.find(".brd.") == std::string::npos) {
      continue;
    }
    const auto status = iterator->symlink_status(error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
      error.clear();
      continue;
    }
    const auto links =
        std::filesystem::hard_link_count(iterator->path(), error);
    if (error || links != 1) {
      error.clear();
      continue;
    }
    const auto fileTime = iterator->last_write_time(error);
    if (error) {
      error.clear();
      continue;
    }
    const auto systemTime =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            fileTime - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    if (systemTime >= cutoff) {
      continue;
    }
    if (std::filesystem::remove(iterator->path(), error) && !error) {
      removedAny = true;
    }
    error.clear();
  }
  if (removedAny) {
    atomic_file::syncDirectory(replayDirectory, diagnostic);
  }
}

} // namespace replay
