#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace replay {

struct ReplayFileMetadata {
  std::filesystem::path relativePath;
  std::string sha256;
  std::uint64_t compressedSize = 0;
  int codecVersion = 0;

  bool operator==(const ReplayFileMetadata &) const = default;
};

struct ReplayFileOwnershipReceipt {
  std::string attemptToken;
  ReplayFileMetadata metadata;

  bool operator==(const ReplayFileOwnershipReceipt &) const = default;
};

enum class ReplayFileLifecycleState : std::uint8_t {
  Reserved,
  TemporaryWritten,
  InstalledUnassociated,
  Associated,
  Finalized,
  Abandoned,
};

struct ReplayFileLifecycle {
  ReplayFileLifecycleState state = ReplayFileLifecycleState::Reserved;
  std::string attemptToken;
  std::optional<ReplayFileOwnershipReceipt> receipt;

  bool operator==(const ReplayFileLifecycle &) const = default;
};

enum class ReplayFileLifecycleEvent : std::uint8_t {
  TemporaryWritten,
  InstallVerified,
  AssociationAcknowledged,
  Finalized,
  Abandon,
};

struct ReplayFileLifecycleTransition {
  std::optional<ReplayFileLifecycle> lifecycle;
  std::string diagnostic;
};

[[nodiscard]] ReplayFileLifecycle
reservedReplayFileLifecycle(std::string attemptToken);

[[nodiscard]] ReplayFileLifecycleTransition advanceReplayFileLifecycle(
    const ReplayFileLifecycle &current, ReplayFileLifecycleEvent event,
    std::optional<ReplayFileOwnershipReceipt> receipt = std::nullopt);

} // namespace replay
