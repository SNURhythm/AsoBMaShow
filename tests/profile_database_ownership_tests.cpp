#include "ProfileDatabaseTools.h"
#include "RAII.h"
#include "sqlite3.h"
#include "support/AllocationFailure.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;

class TempDirectory {
public:
  TempDirectory() {
    for (int attempt = 0; attempt < 32; ++attempt) {
      const auto candidate = fs::temp_directory_path() /
          ("asobmashow-profile-db-ownership-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(attempt));
      if (fs::create_directory(candidate)) {
        path = candidate;
        return;
      }
    }
    throw std::runtime_error("could not claim profile database test directory");
  }
  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
    assert(!error);
  }
  fs::path path;
};

template <typename Operation>
void checkSqliteCleanup(const char *label, Operation operation) {
  operation();
  const auto baseline = sqlite3_memory_used();
  for (std::size_t index = 0; index < 256; ++index) {
    bool threw = false;
    {
      test_support::FailAllocationAfter failure(index);
      try { operation(); }
      catch (const std::bad_alloc &) { threw = true; }
    }
    const auto remaining = sqlite3_memory_used() - baseline;
    if (remaining != 0) {
      std::cerr << label << " allocation " << index << ": retained "
                << remaining << " SQLite bytes\n";
      std::abort();
    }
    if (!threw) {
      assert(index > 0);
      std::cout << label << ": " << index << " allocation failures passed\n";
      operation();
      assert(sqlite3_memory_used() == baseline);
      return;
    }
  }
  assert(false && "allocation walk did not reach a successful return");
}

void testOpenFailure(const fs::path &root) {
  const auto missing = root / "missing.db";
  checkSqliteCleanup("Failed open", [&] {
    std::string error;
    assert(!sqliteIntegrityCheck(missing, error));
    assert(error.find("opening SQLite database failed") != std::string::npos);
  });
}

int denyBegin(void *, int action, const char *operation, const char *,
              const char *, const char *) noexcept {
  return action == SQLITE_TRANSACTION && operation &&
                 std::strcmp(operation, "BEGIN") == 0
             ? SQLITE_DENY : SQLITE_OK;
}

int installAuthorizer(sqlite3 *database, char **,
                       const sqlite3_api_routines *) noexcept {
  return sqlite3_set_authorizer(database, denyBegin, nullptr);
}

void testExecuteFailure(const fs::path &root) {
  const auto source = root / "source.db";
  const auto destination = root / "snapshot.db";
  sqlite3 *raw = nullptr;
  const auto encoded = source.u8string();
  assert(sqlite3_open(reinterpret_cast<const char *>(encoded.c_str()), &raw) == SQLITE_OK);
  UniqueResource<sqlite3, sqlite3_close> database(raw);
  assert(sqlite3_exec(database.get(), "CREATE TABLE records(value); INSERT INTO records VALUES(1)",
                      nullptr, nullptr, nullptr) == SQLITE_OK);
  database.reset();

  assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(installAuthorizer)) == SQLITE_OK);
  const auto resetExtensions = makeScopeExit([] { sqlite3_reset_auto_extension(); });
  checkSqliteCleanup("Failed transaction", [&] {
    std::string error;
    assert(!snapshotSqliteDatabase(source, destination, error));
    assert(error == "starting SQLite snapshot transaction failed: not authorized");
  });
  sqlite3_reset_auto_extension();
  std::string error;
  assert(snapshotSqliteDatabase(source, destination, error));
  assert(error.empty());
  assert(sqliteIntegrityCheck(destination, error));
  assert(sqliteTableRowCount(destination, "records", error) == 1);
}
} // namespace

int main(int argc, char **argv) {
  assert(sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 1) == SQLITE_OK);
  assert(sqlite3_initialize() == SQLITE_OK);
  TempDirectory directory;
  if (argc == 1 || std::strcmp(argv[1], "--open") == 0) testOpenFailure(directory.path);
  if (argc == 1 || std::strcmp(argv[1], "--execute") == 0) testExecuteFailure(directory.path);
  assert(sqlite3_shutdown() == SQLITE_OK);
}
