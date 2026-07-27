#pragma once

#include "../repositories/ReplayRepository.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

enum class ReplayProfileTransferState {
  Succeeded,
  SourceInvalid,
  DestinationInvalid,
  DestinationFailure,
};

struct ReplayProfileTransferFaults {
  std::function<bool(std::string_view)> failAt;
};

struct ReplayProfileTransferOutcome {
  ReplayProfileTransferState state =
      ReplayProfileTransferState::DestinationFailure;
  std::vector<std::filesystem::path> copiedRelativePaths;
  std::size_t omittedMissing = 0;
  std::size_t omittedUserDeleted = 0;
  std::string diagnostic;
};

class ReplayProfileTransfer {
public:
  ReplayProfileTransfer(std::filesystem::path sourceProfileRoot,
                        std::filesystem::path destinationProfileRoot,
                        ReplayProfileTransferFaults faults = {});

  [[nodiscard]] ReplayProfileTransferOutcome
  copy(const std::vector<ModernReplayFileInventoryEntry> &inventory) const;

  // Missing active files are valid: replay bytes are optional companions to
  // durable result history. When rejectUnreferencedFiles is true, every BRD in
  // the destination must be owned by one active inventory entry.
  [[nodiscard]] ReplayProfileTransferOutcome validate(
      const std::vector<ModernReplayFileInventoryEntry> &inventory,
      bool rejectUnreferencedFiles) const;

private:
  std::filesystem::path sourceProfileRoot_;
  std::filesystem::path destinationProfileRoot_;
  ReplayProfileTransferFaults faults_;
};

} // namespace replay
