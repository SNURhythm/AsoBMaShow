#pragma once

#include "../replay/ReplayPlaybackData.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
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

struct ReplayMigrationChartIdentity {
  std::string_view chartPath;
  std::string_view chartMd5;
  std::string_view chartSha256;
};

struct ReplayMigrationChartMetadata {
  int keyMode = 0;
  bool hasUndefinedLongNotes = false;

  bool operator==(const ReplayMigrationChartMetadata &) const = default;
};

using ReplayMigrationChartMetadataResolver = std::function<
    std::optional<ReplayMigrationChartMetadata>(
        const ReplayMigrationChartIdentity &)>;

[[nodiscard]] ReplayMigrationChartMetadataResolver
makeChartDatabaseReplayMetadataResolver(
    const std::filesystem::path &chartDatabasePath);

[[nodiscard]] std::optional<replay::LogicalControl>
legacyReplayControlForPhysicalLane(int physicalLane, int keyMode) noexcept;

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

// Kept with the schema-10 migration so normal runtime sources never depend on
// the retired row-per-event table names.
[[nodiscard]] bool compactReplaySchemaHasNoLegacyPayloadTables(
    sqlite3 *database);

ReplayMigrationOutcome migrateReplaySchema10To11(
    sqlite3 *database, const std::filesystem::path &profileRoot,
    const replay::BeatorajaReplayCodec &codec,
    replay::ReplayFileStore &fileStore, ReplayMigrationFaults faults = {},
    ReplayMigrationChartMetadataResolver resolveMetadata = {});

} // namespace replay_repository_detail
