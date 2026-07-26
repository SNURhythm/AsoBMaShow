#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

bool snapshotSqliteDatabase(const std::filesystem::path &source,
                            const std::filesystem::path &destination,
                            std::string &errorMessage);

bool sqliteIntegrityCheck(const std::filesystem::path &database,
                          std::string &errorMessage);

std::optional<std::int64_t>
sqliteTableRowCount(const std::filesystem::path &database,
                    std::string_view table, std::string &errorMessage);

std::optional<std::int64_t> sqliteStandaloneLegacyReplayRowCount(
    const std::filesystem::path &database, std::string &errorMessage);

std::optional<std::map<std::string, std::int64_t>>
sqliteUserTableRowCounts(const std::filesystem::path &database,
                         std::string &errorMessage);

std::optional<int>
sqliteDatabaseUserVersion(const std::filesystem::path &database,
                          std::string &errorMessage);
