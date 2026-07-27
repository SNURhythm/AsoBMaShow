#pragma once

#include "BeatorajaReplayPath.h"
#include "ReplayFileLifecycle.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

[[nodiscard]] bool
isPrivateReplayTemporaryFilename(std::string_view filename) noexcept;

enum class ReplayFileState {
  Available,
  Missing,
  Corrupt,
  Unsafe,
  IoFailure,
};

struct ReplayFileInspection {
  ReplayFileState state = ReplayFileState::IoFailure;
  std::string diagnostic;
};

struct ReplayFileReadOutcome {
  ReplayFileState state = ReplayFileState::IoFailure;
  std::optional<std::vector<std::byte>> bytes;
  std::string diagnostic;
};

struct ReplayFileReservation {
  ReplayPathIdentity identity;
  std::string attemptToken;
  ReplayFileMetadata expectedMetadata;
  std::filesystem::path temporaryRelativePath;

  bool operator==(const ReplayFileReservation &) const = default;
};

struct ReplayReservationOutcome {
  std::optional<ReplayFileReservation> reservation;
  std::string diagnostic;
};

struct ReplayInstalledFile {
  ReplayFileMetadata metadata;
  std::string attemptToken;
  ReplayFileLifecycle lifecycle;

  bool operator==(const ReplayInstalledFile &) const = default;
};

enum class ReplayInstallState {
  InstalledVerified,
  RetryableAmbiguous,
  Occupied,
  Unsafe,
  Failed,
};

struct ReplayInstallOutcome {
  ReplayInstallState state = ReplayInstallState::Failed;
  std::optional<ReplayInstalledFile> file;
  bool existingIdenticalFile = false;
  std::string diagnostic;
};

struct ReplayFileStoreFaults {
  std::function<bool(std::string_view)> failAt;
};

class ReplayFileStore {
public:
  explicit ReplayFileStore(std::filesystem::path profileRoot,
                           ReplayFileStoreFaults faults = {},
                           ReplayLimits limits = kReplayLimits);

  [[nodiscard]] ReplayReservationOutcome
  reserve(const ReplayPathIdentity &identity, std::span<const std::byte> bytes,
          std::string_view attemptToken) const;

  [[nodiscard]] ReplayInstallOutcome
  install(const ReplayFileReservation &reservation,
          std::span<const std::byte> bytes) const;

  [[nodiscard]] ReplayFileInspection
  inspect(const ReplayFileMetadata &metadata) const;

  [[nodiscard]] ReplayFileReadOutcome
  readVerified(const ReplayFileMetadata &metadata) const;

  bool removeIfMatches(const ReplayFileMetadata &metadata,
                       std::string &diagnostic) const;

  bool removeReferencedEntry(const ReplayFileMetadata &metadata,
                             std::string &diagnostic) const;

  void
  removeStaleTemporaryFiles(std::chrono::system_clock::time_point cutoff) const;

private:
  std::filesystem::path profileRoot_;
  ReplayFileStoreFaults faults_;
  ReplayLimits limits_;
};

} // namespace replay
