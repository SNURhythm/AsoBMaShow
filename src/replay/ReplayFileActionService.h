#pragma once

#include "ReplayFileStore.h"
#include "../repositories/ReplayRepository.h"

#include <filesystem>
#include <optional>
#include <string>

enum class ReplayAvailability {
  Available,
  Missing,
  Corrupt,
  Unsafe,
  IoFailure,
};

struct ReplayFileActionOutcome {
  ReplayAvailability availability = ReplayAvailability::IoFailure;
  bool changed = false;
  std::optional<std::filesystem::path> sourcePath;
  std::string suggestedFilename;
  std::string diagnostic;
};

class ReplayFileActionService {
public:
  ReplayFileActionService(ReplayRepository &repository,
                          replay::ReplayFileStore &fileStore);

  [[nodiscard]] ReplayFileActionOutcome inspect(LocalResultRecordId record);
  [[nodiscard]] ReplayFileActionOutcome remove(LocalResultRecordId record);
  [[nodiscard]] ReplayFileActionOutcome
  copyToBeatorajaSlot(LocalResultRecordId record, int slot);

private:
  struct ResolvedReference {
    ReplayFileReference reference;
    replay::ReplayFileMetadata metadata;
  };

  [[nodiscard]] std::optional<ResolvedReference>
  resolve(LocalResultRecordId record, ReplayFileActionOutcome &outcome);
  [[nodiscard]] ReplayFileActionOutcome
  inspectResolved(const ResolvedReference &resolved) const;

  ReplayRepository &repository_;
  replay::ReplayFileStore &fileStore_;
};
