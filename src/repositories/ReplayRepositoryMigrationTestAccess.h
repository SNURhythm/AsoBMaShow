#pragma once

struct sqlite3;

namespace replay_repository_test {

enum class PathMigrationFault {
  None,
  SnapshotCopy,
  SchemaMigration,
  Compaction,
  Installation,
};

// Test-only seam for transaction fault injection around the schema owner.
// Production callers migrate through ReplayRepository::EnsureSchema().
[[nodiscard]] bool RunSchemaMigration(sqlite3 *database);
void SetPathMigrationFault(PathMigrationFault fault);

} // namespace replay_repository_test
