#pragma once

#include "ReplayFileStore.h"
#include "../repositories/ReplayRepository.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
  std::shared_ptr<void> sourceLifetime;
  std::string suggestedFilename;
  std::string diagnostic;
};

class ReplayFileActionService {
public:
  ReplayFileActionService(ReplayRepository &repository,
                          replay::ReplayFileStore &fileStore);

  [[nodiscard]] ReplayFileActionOutcome inspect(LocalResultRecordId record);
  [[nodiscard]] ReplayFileActionOutcome
  prepareShare(LocalResultRecordId record);
  [[nodiscard]] ReplayFileActionOutcome remove(LocalResultRecordId record);
  [[nodiscard]] ReplayFileActionOutcome
  copyToBeatorajaSlot(LocalResultRecordId record, int slot);

private:
  struct StageContext {
    std::string chartSha256;
    int keyMode = 0;
    int longNoteMode = 0;
  };

  struct ResolvedReference {
    ReplayFileReference reference;
    replay::ReplayFileMetadata metadata;
    bool course = false;
    std::vector<StageContext> stages;
    int courseLongNoteMode = 0;
    std::vector<int> courseConstraintIds;
  };

  [[nodiscard]] std::optional<ResolvedReference>
  resolve(LocalResultRecordId record, ReplayFileActionOutcome &outcome);
  [[nodiscard]] ReplayFileActionOutcome
  inspectResolved(const ResolvedReference &resolved) const;

  ReplayRepository &repository_;
  replay::ReplayFileStore &fileStore_;
};
