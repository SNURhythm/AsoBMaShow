#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

struct sqlite3;

namespace replay {
class BeatorajaReplayCodec;
class ReplayFileStore;
} // namespace replay

namespace replay_repository_detail {

struct ReplayMigrationFaults {
  std::function<bool(std::string_view phase, std::int64_t publicId)> failAt;
};

struct ReplayMigrationOutcome {
  enum class Status {
    Migrated,
    AlreadyCurrent,
    InvalidLegacyData,
    FileFailure,
    StorageFailure,
  };

  Status status = Status::StorageFailure;
  std::size_t chartFiles = 0;
  std::size_t courseFiles = 0;
  std::string diagnostic;
};

ReplayMigrationOutcome migrateReplaySchema10To11(
    sqlite3 *database, const std::filesystem::path &profileRoot,
    const replay::BeatorajaReplayCodec &codec,
    replay::ReplayFileStore &fileStore, ReplayMigrationFaults faults = {});

} // namespace replay_repository_detail
