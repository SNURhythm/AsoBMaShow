#pragma once

struct sqlite3;

namespace replay_repository_test {

// Test-only seam for transaction fault injection around the schema owner.
// Production callers migrate through ReplayRepository::EnsureSchema().
[[nodiscard]] bool RunSchemaMigration(sqlite3 *database);

} // namespace replay_repository_test
