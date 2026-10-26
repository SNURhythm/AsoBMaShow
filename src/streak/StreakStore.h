#pragma once
#include "StreakModel.h"
#include <filesystem>
struct sqlite3;
namespace streak {
// These functions never start a transaction. The caller owns the score/config
// transaction, so a failed state write also rolls back the result projection.
State load(sqlite3 *database);
void save(sqlite3 *database, const State &state);
void recordSavedPlay(sqlite3 *database, const std::filesystem::path &chartDatabase,
                     Play play, const std::string &md5,
                     const std::string &createdAt, std::uint32_t randomChoice);
}
