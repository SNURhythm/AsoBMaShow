#include "../src/AppSettingsStore.h"
#include "../src/ArchiveRAII.h"
#include "../src/AtomicFile.h"
#include "../src/FileChecksum.h"
#include "../src/PlayerProfileManager.h"
#include "../src/ProfileArchive.h"
#include "../src/ProfileDatabaseTools.h"
#include "../src/Uuid.h"
#include "../src/ir/PendingIrCredentialCleanup.h"
#include "../src/practice/PracticePresetStore.h"
#include "../src/repositories/ReplayRepository.h"
#include "../src/repositories/ScoreRepository.h"
#include "../src/replay/ReplayFileStore.h"
#include "../src/input/InputProfileStore.h"
#include "../src/sqlite3.h"
#include "../src/skin/package/SkinPathPolicy.h"
#include "../yoga/lib/nlohmann/json.hpp"
#include "ReplayLegacyFixture.h"

#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <AclAPI.h>
#endif

namespace {
using Json = nlohmann::json;

constexpr std::string_view kPracticeHash = "0123456789abcdef0123456789abcdef"
                                           "0123456789abcdef0123456789abcdef";
constexpr std::array<std::string_view, 7> kExpectedMembers = {
    "manifest.json",
    "settings.json",
    "input.json",
    "scores.db",
    "replays.db",
    "practice/"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.json",
    "checksums.sha256"};

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  explicit TempDirectory(
      std::string_view label = "archive",
      std::optional<std::filesystem::path> firstCandidate = std::nullopt) {
    static std::atomic<unsigned long long> sequence{0};
    constexpr unsigned int kMaxAttempts = 32;
    for (unsigned int attempt = 0; attempt < kMaxAttempts; ++attempt) {
      const auto candidate =
          firstCandidate && attempt == 0
              ? *firstCandidate
              : std::filesystem::temp_directory_path() /
                    ("asobmashow-" + std::string(label) + "-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()) +
                     "-" + std::to_string(sequence.fetch_add(1)));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
      if (error && error != std::errc::file_exists) {
        throw std::filesystem::filesystem_error(
            "create temporary directory", candidate, error);
      }
    }
    throw std::filesystem::filesystem_error(
        "create unique temporary directory",
        std::filesystem::temp_directory_path(),
        std::make_error_code(std::errc::file_exists));
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void testTempDirectoryRetriesOccupiedCandidate() {
  TempDirectory parent("temp-directory-collision-parent");
  const auto occupied =
      parent.path() / "asobmashow-temp-directory-archive-collision-0-0";
  std::error_code error;
  std::filesystem::remove_all(occupied, error);
  std::filesystem::create_directory(occupied, error);
  expect(!error, "temporary directory collision fixture creates");
  if (error) {
    return;
  }

  {
    TempDirectory temp("temp-directory-collision", occupied);
    expect(temp.path() != occupied,
           "temporary directory retries an occupied candidate");
    expect(std::filesystem::is_directory(temp.path()),
           "temporary directory claims a new private root");
  }

  std::filesystem::remove_all(occupied, error);
  expect(!error, "temporary directory collision fixture cleans up");
}

void writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

bool isLowerHexToken(std::string_view token) {
  return token.size() == 32 &&
         std::ranges::all_of(token, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::string generateWorkspaceToken() {
  std::string token = uuid::generateV4();
  token.erase(std::remove(token.begin(), token.end(), '-'), token.end());
  return token;
}

class ScopedDirectoryLock {
public:
  explicit ScopedDirectoryLock(const std::filesystem::path &path)
      : path_(path) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        locked_ = true;
        return;
      }
      if (error && error != std::errc::file_exists) {
        expect(false, "global import-workspace test lock creates: " +
                          error.message());
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(false,
           "global import-workspace test lock is acquired within 30 seconds");
  }

  ~ScopedDirectoryLock() {
    if (locked_) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  [[nodiscard]] bool locked() const { return locked_; }

private:
  std::filesystem::path path_;
  bool locked_ = false;
};

std::uint16_t readLittleEndian16(std::string_view bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(
      static_cast<unsigned char>(bytes[offset]) |
      (static_cast<unsigned char>(bytes[offset + 1]) << 8U));
}

void writeLittleEndian32(std::string &bytes, std::size_t offset,
                         std::uint32_t value) {
  for (int byte = 0; byte < 4; ++byte) {
    bytes[offset + byte] = static_cast<char>((value >> (byte * 8U)) & 0xffU);
  }
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

#ifdef _WIN32
bool hasProtectedCurrentUserOnlyDacl(const std::filesystem::path &path) {
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD securityResult = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
  if (securityResult != ERROR_SUCCESS || descriptor == nullptr ||
      dacl == nullptr || dacl->AceCount != 1) {
    if (descriptor != nullptr) {
      LocalFree(descriptor);
    }
    return false;
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  void *rawAce = nullptr;
  HANDLE token = nullptr;
  DWORD tokenBytes = 0;
  bool privateAcl =
      GetSecurityDescriptorControl(descriptor, &control, &revision) &&
      (control & SE_DACL_PROTECTED) != 0 && GetAce(dacl, 0, &rawAce) &&
      rawAce != nullptr &&
      static_cast<ACE_HEADER *>(rawAce)->AceType == ACCESS_ALLOWED_ACE_TYPE &&
      OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
  std::vector<std::byte> tokenUser;
  if (privateAcl) {
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
    tokenUser.resize(tokenBytes);
    privateAcl = tokenBytes > 0 &&
                 GetTokenInformation(token, TokenUser, tokenUser.data(),
                                     tokenBytes, &tokenBytes);
  }
  if (privateAcl) {
    auto *user = reinterpret_cast<TOKEN_USER *>(tokenUser.data());
    auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
    privateAcl = EqualSid(user->User.Sid, &ace->SidStart) != FALSE;
  }
  if (token != nullptr) {
    CloseHandle(token);
  }
  LocalFree(descriptor);
  return privateAcl;
}
#endif

bool patchZipDeclaredSize(const std::filesystem::path &path,
                          std::string_view memberName,
                          std::uint32_t declaredSize) {
  std::string bytes = readFile(path);
  bool patchedCentralDirectory = false;
  for (std::size_t offset = 0; offset + 46 <= bytes.size(); ++offset) {
    const auto signature = std::string_view(bytes).substr(offset, 4);
    if (signature == std::string_view("PK\x01\x02", 4)) {
      const std::size_t nameLength = readLittleEndian16(bytes, offset + 28);
      const std::size_t extraLength = readLittleEndian16(bytes, offset + 30);
      const std::size_t commentLength = readLittleEndian16(bytes, offset + 32);
      if (offset + 46 + nameLength + extraLength + commentLength >
          bytes.size()) {
        return false;
      }
      if (std::string_view(bytes).substr(offset + 46, nameLength) ==
          memberName) {
        writeLittleEndian32(bytes, offset + 24, declaredSize);
        patchedCentralDirectory = true;
      }
      offset += 45 + nameLength + extraLength + commentLength;
      continue;
    }
    if (signature == std::string_view("PK\x03\x04", 4)) {
      const std::size_t nameLength = readLittleEndian16(bytes, offset + 26);
      const std::size_t extraLength = readLittleEndian16(bytes, offset + 28);
      if (offset + 30 + nameLength + extraLength > bytes.size()) {
        return false;
      }
      if (std::string_view(bytes).substr(offset + 30, nameLength) ==
          memberName) {
        writeLittleEndian32(bytes, offset + 22, declaredSize);
      }
    }
  }
  if (patchedCentralDirectory) {
    writeFile(path, bytes);
  }
  return patchedCentralDirectory;
}

bool execute(sqlite3 *database, const std::string &sql) {
  char *error = nullptr;
  const int rc = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    std::cerr << "SQL failure: " << (error == nullptr ? "unknown" : error)
              << '\n';
    sqlite3_free(error);
    return false;
  }
  return true;
}

struct DatabaseCloser {
  void operator()(sqlite3 *database) const {
    if (database != nullptr) {
      sqlite3_close(database);
    }
  }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

Database openDatabase(const std::filesystem::path &path) {
  sqlite3 *raw = nullptr;
  if (sqlite3_open(path.string().c_str(), &raw) != SQLITE_OK) {
    if (raw != nullptr) {
      sqlite3_close(raw);
    }
    return {};
  }
  return Database(raw);
}

void setReplayCodecVersion(const std::filesystem::path &path,
                           std::int64_t referenceId, int codecVersion) {
  auto database = openDatabase(path);
  expect(database != nullptr, "future-codec archive database opens");
  const std::string sql = "UPDATE modern_replay_files SET codec_version=" +
                          std::to_string(codecVersion) +
                          " WHERE id=" + std::to_string(referenceId);
  expect(database && execute(database.get(), sql) &&
             sqlite3_changes(database.get()) == 1,
         "future-codec archive metadata updates within the schema contract");
}

void setDatabaseVersion(const std::filesystem::path &path, int version,
                        std::string_view label) {
  Database database = openDatabase(path);
  expect(database != nullptr, std::string(label) + " database opens");
  if (!database) {
    return;
  }
  expect(
      execute(database.get(), "PRAGMA user_version=" + std::to_string(version)),
      std::string(label) + " database version updates");
}

std::int64_t rowCount(const std::filesystem::path &path,
                      std::string_view table) {
  std::string error;
  const auto result = sqliteTableRowCount(path, table, error);
  expect(result.has_value(), "row count succeeds: " + error);
  return result.value_or(-1);
}

std::int64_t matchingRowCount(const std::filesystem::path &database,
                              std::string_view query) {
  Database connection = openDatabase(database);
  expect(connection != nullptr, "matching row count database opens");
  if (!connection) {
    return -1;
  }
  sqlite3_stmt *raw = nullptr;
  const std::string sql(query);
  if (sqlite3_prepare_v2(connection.get(), sql.c_str(), -1, &raw, nullptr) !=
      SQLITE_OK) {
    expect(false, "matching row count query prepares");
    return -1;
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      raw, sqlite3_finalize);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    expect(false, "matching row count query returns a row");
    return -1;
  }
  return sqlite3_column_int64(statement.get(), 0);
}

result_persistence::ModernChartResult portableReplayResult(int suffix,
                                                           char hash);

void seedIrOperationalState(const std::filesystem::path &path,
                            std::string_view label) {
  ReplayRepository repository(path);
  expect(repository.EnsureSchema(),
         std::string(label) + " IR outbox schema initializes");
  const auto modern = portableReplayResult(9, 'd');
  const auto staged =
      repository.StageModernChartResult(modern, std::nullopt, std::nullopt, {});
  expect((staged.status == ModernChartStageStatus::Staged ||
          staged.status == ModernChartStageStatus::AlreadyStaged) &&
             staged.receipt,
         std::string(label) + " modern IR receipt owner stages");
  const int modernResultId =
      staged.receipt.has_value() ? staged.receipt->resultId : 0;
  repository.Shutdown();
  Database database = openDatabase(path);
  expect(database != nullptr, std::string(label) + " IR outbox database opens");
  if (!database) {
    return;
  }
  expect(
      execute(
          database.get(),
          "INSERT INTO ir_outbox(provider_id,attempt_id,chart_md5,"
          "chart_sha256,payload_json,ruleset_id,ruleset_revision,"
          "validation_fingerprint,state,local_result_ready,"
          "next_attempt_at_ms,remote_job_id,remote_origin,created_at_ms,"
          "updated_at_ms,completed_at_ms) VALUES"
          "('tachi','10000000-0000-4000-8000-000000000001',NULL,"
          "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
          "'{\"score\":1}','test-rules',1,lower(hex(zeroblob(32))),"
          "0,1,NULL,NULL,NULL,1000,1000,NULL),"
          "('archive_readonly','10000000-0000-4000-8000-000000000002',NULL,"
          "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
          "'{\"score\":2}','test-rules',1,lower(hex(zeroblob(32))),"
          "2,1,3000,'job-2','https://example.invalid',"
          "2000,2000,NULL),"
          "('tachi_backup','10000000-0000-4000-8000-000000000003',NULL,"
          "'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc',"
          "'{\"score\":3}','test-rules',1,lower(hex(zeroblob(32))),"
          "5,1,NULL,NULL,NULL,3000,3000,3000)"),
      std::string(label) + " pending, deferred, and succeeded IR rows seed");
  expect(
      execute(
          database.get(),
          "INSERT INTO ir_submission_receipts(provider_id,server_origin,"
          "modern_chart_result_id,attempt_id,chart_md5,chart_sha256,"
          "confirmation_source,confirmed_at_ms) VALUES('tachi',"
          "'https://boku.tachi.ac'," +
              std::to_string(modernResultId) + ", '" + modern.attemptId +
              "','" + modern.score.chartMd5 + "','" + modern.score.chartSha256 +
              "',"
              "1,4000);"
              "INSERT INTO ir_remote_scores(provider_id,server_origin,"
              "remote_score_id,remote_user_id,game,remote_chart_id,chart_md5,"
              "chart_sha256,title,artist,note_count,score,lamp_rank,service,"
              "time_added_ms,sync_generation) VALUES('tachi',"
              "'https://"
              "boku.tachi.ac','remote-score',42,'bms-7k','remote-chart',"
              "'dddddddddddddddddddddddddddddddd',"
              "'ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
              "d',"
              "'Remote Song','Remote Artist',1,1,0,'Bokutachi',4000,1)"),
      std::string(label) + " account-scoped IR evidence and mirror seed");
}

void seedImportedIrScore(const std::filesystem::path &path,
                         std::string_view label) {
  Database database = openDatabase(path);
  expect(database != nullptr,
         std::string(label) + " imported IR score database opens");
  if (!database) {
    return;
  }
  expect(
      execute(
          database.get(),
          "INSERT INTO scores(chart_sha256,score,max_score,max_combo,"
          "combo_break,pgreat,great,good,bad,poor,kpoor,fast,slow,"
          "final_gauge,clear_type,score_source,source_provider_id,"
          "source_server_origin,source_remote_score_id,"
          "source_sync_generation) VALUES("
          "'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee',"
          "1,2,1,0,0,1,0,0,0,0,0,0,0.0,0,1,'tachi',"
          "'https://boku.tachi.ac','remote-score',1)"),
      std::string(label) + " imported IR score projection seeds");
}

std::string scalarText(const std::filesystem::path &path,
                       std::string_view query) {
  Database database = openDatabase(path);
  expect(database != nullptr, "scalar query database opens");
  if (!database) {
    return {};
  }
  sqlite3_stmt *raw = nullptr;
  if (sqlite3_prepare_v2(database.get(), std::string(query).c_str(), -1, &raw,
                         nullptr) != SQLITE_OK) {
    expect(false, "scalar query prepares");
    return {};
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      raw, sqlite3_finalize);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    expect(false, "scalar query returns a row");
    return {};
  }
  const auto *text = sqlite3_column_text(statement.get(), 0);
  return text == nullptr ? std::string{}
                         : std::string(reinterpret_cast<const char *>(text));
}

struct ArchiveMember {
  std::string name;
  std::string contents;
  mode_t type = AE_IFREG;
  std::optional<std::string> symlink;
  std::optional<std::string> hardlink;
  std::int64_t sizeOverride = -1;
  std::int64_t mtime = 0;
};

using ArchiveEntryHandle =
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;

bool writeArchive(const std::filesystem::path &path,
                  const std::vector<ArchiveMember> &members,
                  std::string &errorMessage) {
  auto writer = makeArchiveWriteHandle();
  if (!writer || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
      archive_write_set_options(writer.get(), "zip:compression=store") !=
          ARCHIVE_OK ||
      archive_write_open_filename(writer.get(), path.string().c_str()) !=
          ARCHIVE_OK) {
    errorMessage = writer ? archive_error_string(writer.get())
                          : "unable to allocate archive writer";
    return false;
  }
  for (const ArchiveMember &member : members) {
    ArchiveEntryHandle entry(archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), member.name.c_str());
    archive_entry_set_filetype(entry.get(), member.type);
    archive_entry_set_perm(entry.get(), member.type == AE_IFDIR ? 0755 : 0644);
    archive_entry_set_uid(entry.get(), 0);
    archive_entry_set_gid(entry.get(), 0);
    archive_entry_set_mtime(entry.get(), member.mtime, 0);
    archive_entry_set_size(
        entry.get(), member.sizeOverride >= 0
                         ? member.sizeOverride
                         : static_cast<la_int64_t>(member.contents.size()));
    if (member.symlink) {
      archive_entry_set_symlink(entry.get(), member.symlink->c_str());
    }
    if (member.hardlink) {
      archive_entry_set_hardlink(entry.get(), member.hardlink->c_str());
    }
    if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK) {
      errorMessage = archive_error_string(writer.get());
      return false;
    }
    if (!member.contents.empty()) {
      const auto written = archive_write_data(
          writer.get(), member.contents.data(), member.contents.size());
      if (written != static_cast<la_ssize_t>(member.contents.size())) {
        errorMessage = archive_error_string(writer.get());
        return false;
      }
    }
    if (archive_write_finish_entry(writer.get()) != ARCHIVE_OK) {
      errorMessage = archive_error_string(writer.get());
      return false;
    }
  }
  if (archive_write_close(writer.get()) != ARCHIVE_OK) {
    errorMessage = archive_error_string(writer.get());
    return false;
  }
  return true;
}

std::vector<ArchiveMember> readArchive(const std::filesystem::path &path,
                                       std::string &errorMessage) {
  std::vector<ArchiveMember> members;
  auto reader = makeArchiveReadHandle();
  if (!reader || archive_read_support_filter_none(reader.get()) != ARCHIVE_OK ||
      archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
      archive_read_open_filename(reader.get(), path.string().c_str(), 65536) !=
          ARCHIVE_OK) {
    errorMessage = reader ? archive_error_string(reader.get())
                          : "unable to allocate archive reader";
    return {};
  }
  archive_entry *entry = nullptr;
  while (archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK) {
    const char *name = archive_entry_pathname(entry);
    ArchiveMember member;
    member.name = name == nullptr ? std::string{} : std::string(name);
    member.type = archive_entry_filetype(entry);
    member.mtime = archive_entry_mtime(entry);
    if (const char *link = archive_entry_symlink(entry); link != nullptr) {
      member.symlink = link;
    }
    if (const char *link = archive_entry_hardlink(entry); link != nullptr) {
      member.hardlink = link;
    }
    std::array<char, 65536> buffer{};
    while (true) {
      const la_ssize_t count =
          archive_read_data(reader.get(), buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
        errorMessage = archive_error_string(reader.get());
        return {};
      }
      member.contents.append(buffer.data(), static_cast<std::size_t>(count));
    }
    members.push_back(std::move(member));
  }
  if (archive_errno(reader.get()) != 0) {
    errorMessage = archive_error_string(reader.get());
  }
  return members;
}

ArchiveMember *findMember(std::vector<ArchiveMember> &members,
                          std::string_view name) {
  const auto found = std::ranges::find(members, name, &ArchiveMember::name);
  return found == members.end() ? nullptr : &*found;
}

std::string canonicalChecksums(const std::vector<ArchiveMember> &members,
                               bool includeReplayFiles = false) {
  std::string result;
  for (const auto &member : members) {
    if (member.name == "checksums.sha256" ||
        (!includeReplayFiles && member.name.starts_with("replay/"))) {
      continue;
    }
    result += file_checksum::sha256(member.contents);
    result += "  ";
    result += member.name;
    result += '\n';
  }
  return result;
}

void refreshChecksums(std::vector<ArchiveMember> &members) {
  if (ArchiveMember *checksums = findMember(members, "checksums.sha256")) {
    checksums->contents = canonicalChecksums(members);
  }
}

std::vector<std::filesystem::path>
transactionArtifacts(const std::filesystem::path &applicationRoot) {
  std::vector<std::filesystem::path> result;
  std::error_code error;
  const auto profiles = applicationRoot / "profiles";
  if (!std::filesystem::exists(profiles, error)) {
    return result;
  }
  for (const auto &entry : std::filesystem::directory_iterator(profiles)) {
    const std::string name = entry.path().filename().string();
    if (name.starts_with(".staging-") || name.starts_with(".backup-")) {
      result.push_back(entry.path());
    }
  }
  return result;
}

struct Fixture {
  TempDirectory temp{"profile-archive"};
  TempDirectory exchange{"profile-archive-exchange"};
  std::vector<std::string> uuids{"11111111-1111-4111-8111-111111111111",
                                 "22222222-2222-4222-8222-222222222222",
                                 "33333333-3333-4333-8333-333333333333",
                                 "44444444-4444-4444-8444-444444444444",
                                 "55555555-5555-4555-8555-555555555555",
                                 "66666666-6666-4666-8666-666666666666",
                                 "77777777-7777-4777-8777-777777777777",
                                 "88888888-8888-4888-8888-888888888888",
                                 "99999999-9999-4999-8999-999999999999",
                                 "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"};
  std::size_t uuidIndex = 0;
  PlayerProfileManager manager;
  std::string sourceId;
  std::string targetId;
  std::filesystem::path baselineArchivePath;

private:
  struct SeedFixtureTag {};

  Fixture(SeedFixtureTag) : manager(temp.path(), dependencies({})) {
    ++fixtureSeedBuildCount();
    const auto initialized = manager.Initialize();
    expect(initialized.ok(),
           "archive fixture initializes: " + initialized.message);
    const auto source = manager.createProfile("Portable Profile");
    const auto target = manager.createProfile("Overwrite Target");
    expect(source.ok() && source.profile.has_value(),
           "archive source profile creates");
    expect(target.ok() && target.profile.has_value(),
           "archive target profile creates");
    if (!source.profile || !target.profile) {
      return;
    }
    sourceId = source.profile->id;
    targetId = target.profile->id;

    AppSettings settings;
    settings.audioOffsetMs = -37;
    settings.setVisibleTimeGreenNumber(765);
    settings.selectedGameplayRuleset = "beatoraja";
    settings.selectedPlayOption = "R-RANDOM";
    const auto skinPackage = skin::normalizePackageId("PortableSkin").package;
    const auto skinEntry =
        skinPackage
            ? skin::normalizeEntryPath(*skinPackage, "play/main.luaskin").entry
            : std::nullopt;
    if (skinEntry) {
      auto &entrySettings = settings.skin.entries[*skinEntry];
      entrySettings.options["Lane cover"] = 4;
      entrySettings.offsets["Judge"] = {.x = 12, .y = -9, .a = 32};
      entrySettings.viewport = {
          .mode = skin::ViewportMode::Stretch,
          .customBase = skin::CustomViewportBase::Fit,
      };
      settings.skin.selected7KeyEntry = *skinEntry;
      settings.skin.gameplayCompatibilityEnabled = true;
    }
    settings.sanitize();
    std::string error;
    expect(AppSettingsStore::Save(manager.pathsFor(sourceId).settingsJson,
                                  settings, error),
           "archive source settings save: " + error);

    seedMarker(manager.pathsFor(sourceId).scoresDb, "score-source");
    seedMarker(manager.pathsFor(sourceId).replaysDb, "replay-source");
    seedMarker(manager.pathsFor(targetId).scoresDb, "score-target");
    seedMarker(manager.pathsFor(targetId).replaysDb, "replay-target");
    seedProvenance(manager.pathsFor(sourceId));
    practice::PresetStore presetStore(
        manager.pathsFor(sourceId).practiceDirectory);
    practice::Configuration configuration{
        .chartSha256 = std::string(kPracticeHash),
        .startMicros = 1'000'000,
        .endMicros = 9'000'000,
        .loop = true,
        .gaugeType = GaugeType::Normal,
        .playback = {.percent = 100},
    };
    expect(presetStore.saveLastUsed(kPracticeHash, configuration, error),
           "archive source practice preset saves: " + error);
    expect(manager.validateProfile(sourceId).ok(),
           "archive source remains valid after seeding");
    expect(manager.validateProfile(targetId).ok(),
           "archive target remains valid after seeding");

    baselineArchivePath = exchange.path() / "baseline.asobprofile";
    ProfileArchiveService service(manager);
    ++baselineArchiveExportCount();
    const auto exported = service.Export(sourceId, baselineArchivePath);
    expect(exported.ok(),
           "archive fixture baseline export succeeds: " + exported.message);
  }

  PlayerProfileManagerDependencies
  dependencies(PlayerProfileFilesystemOperations filesystem) {
    PlayerProfileManagerDependencies result;
    result.generateUuid = [this] { return uuids.at(uuidIndex++); };
    result.utcNow = [] { return std::string("2026-07-11T01:23:45Z"); };
    result.filesystem = std::move(filesystem);
    return result;
  }

  static std::size_t &fixtureSeedBuildCount() {
    static std::size_t count = 0;
    return count;
  }

  static std::size_t &baselineArchiveExportCount() {
    static std::size_t count = 0;
    return count;
  }

  static const Fixture &fixtureSeed() {
    static const Fixture seed(SeedFixtureTag{});
    return seed;
  }

  static bool copyDirectoryContents(const std::filesystem::path &source,
                                    const std::filesystem::path &destination) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(source, error);
    if (error) {
      expect(false, "archive fixture seed directory opens: " +
                        error.message());
      return false;
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
      const auto entry = *iterator;
      std::filesystem::copy(entry.path(),
                            destination / entry.path().filename(),
                            std::filesystem::copy_options::recursive, error);
      if (error) {
        expect(false, "archive fixture seed copy succeeds: " +
                          error.message());
        return false;
      }
      iterator.increment(error);
      if (error) {
        expect(false, "archive fixture seed directory iterates: " +
                          error.message());
        return false;
      }
    }
    return true;
  }

public:
  explicit Fixture(PlayerProfileFilesystemOperations filesystem = {})
      : manager(temp.path(), dependencies(std::move(filesystem))) {
    const Fixture &seed = fixtureSeed();
    uuidIndex = seed.uuidIndex;
    if (!copyDirectoryContents(seed.temp.path(), temp.path())) {
      return;
    }
    const auto initialized = manager.Initialize();
    expect(initialized.ok(),
           "archive fixture clone initializes: " + initialized.message);
    if (!initialized.ok()) {
      return;
    }
    sourceId = seed.sourceId;
    targetId = seed.targetId;
  }

  static std::size_t seedBuildCountForTesting() {
    return fixtureSeedBuildCount();
  }

  static std::size_t baselineArchiveBuildCountForTesting() {
    return baselineArchiveExportCount();
  }

  static bool copyProfileTree(const std::filesystem::path &source,
                              const std::filesystem::path &destination) {
    return copyDirectoryContents(source, destination);
  }

  static std::filesystem::path copyBaselineArchive(Fixture &fixture,
                                                    std::string_view name) {
    const Fixture &seed = fixtureSeed();
    const auto archive = fixture.exchange.path() / name;
    std::error_code error;
    std::filesystem::copy_file(seed.baselineArchivePath, archive,
                               std::filesystem::copy_options::none, error);
    expect(!error, "archive fixture baseline archive copies: " +
                       error.message());
    return archive;
  }

  static void seedMarker(const std::filesystem::path &path,
                         std::string_view value) {
    Database database = openDatabase(path);
    expect(database != nullptr, "marker database opens");
    if (!database) {
      return;
    }
    expect(execute(database.get(),
                   "CREATE TABLE IF NOT EXISTS archive_marker(value TEXT)"),
           "marker table creates");
    expect(execute(database.get(), "DELETE FROM archive_marker"),
           "marker rows clear");
    expect(
        execute(database.get(), "INSERT INTO archive_marker(value) VALUES ('" +
                                    std::string(value) + "')"),
        "marker row inserts");
  }

  static void seedProvenance(const PlayerProfilePaths &paths) {
    constexpr std::string_view provenance =
        R"({"schemaVersion":1,"ruleset":{"version":73},"stages":[],"eligibility":"eligible"})";
    Database scores = openDatabase(paths.scoresDb);
    Database replays = openDatabase(paths.replaysDb);
    expect(scores != nullptr && replays != nullptr,
           "provenance databases open");
    if (!scores || !replays) {
      return;
    }
    expect(execute(
               scores.get(),
               "INSERT INTO scores (chart_path,chart_md5,chart_sha256,ln_mode,"
               "chart_title,chart_artist,score,max_score,max_combo,combo_break,"
               "pgreat,great,good,bad,poor,kpoor,fast,slow,final_gauge,"
               "clear_type,ruleset_version,eligibility,provenance_json) VALUES "
               "('portable.bms','0123456789abcdef0123456789abcdef',"
               "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
               "aa',"
               "0,'Portable Song','Portable Artist',123,456,7,8,9,10,11,12,"
               "13,14,15,16,0.75,2,73,0,'" +
                   std::string(provenance) + "')"),
           "score provenance row inserts");
    expect(execute(
               replays.get(),
               "INSERT INTO legacy_chart_result_summaries("
               "legacy_replay_id,chart_path,chart_md5,chart_sha256,chart_title,"
               "chart_artist,long_note_mode,final_score,max_combo,final_gauge,"
               "clear_type,created_at,ruleset_version,eligibility,"
               "provenance_json,partial) VALUES(1,'portable.bms',"
               "'0123456789abcdef0123456789abcdef',"
               "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
               "aa',"
               "'Portable Song','Portable Artist',0,123,7,0.75,2,"
               "'2026-07-11 01:23:45',73,0,'" +
                   std::string(provenance) +
                   "',0);"
                   "INSERT INTO legacy_course_result_summaries("
                   "legacy_course_replay_id,legacy_course_id,course_key,"
                   "course_name,course_group_name,constraint_json,final_score,"
                   "max_combo,final_gauge,clear_type,completed_charts,total_"
                   "charts,"
                   "created_at,ruleset_version,eligibility,provenance_json,"
                   "partial)"
                   " VALUES(2,7,NULL,'Portable Course','Archive Contract','[]',"
                   "321,NULL,0.5,2,1,2,'2026-07-11 01:24:45',73,0,'" +
                   std::string(provenance) + "',1)"),
           "legacy chart and partial course summaries insert");
  }
};

struct FaultArchiveFixtureSeed {
  TempDirectory temp;
  TempDirectory exchange;
  std::filesystem::path archive;

  FaultArchiveFixtureSeed(std::string_view label, std::string_view sourceName,
                          std::string_view sourceMarker,
                          std::optional<std::string_view> targetName = {},
                          std::string_view targetMarker = {})
      : temp(std::string(label) + "-seed"),
        exchange(std::string(label) + "-seed-exchange") {
    const std::string activeId = "11111111-1111-4111-8111-111111111111";
    const std::string sourceId = "22222222-2222-4222-8222-222222222222";
    const std::string targetId = "33333333-3333-4333-8333-333333333333";
    std::vector<std::string> uuids{activeId, sourceId};
    if (targetName) {
      uuids.push_back(targetId);
    }
    std::size_t uuidIndex = 0;
    PlayerProfileManagerDependencies dependencies;
    dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
    dependencies.utcNow = [] { return std::string("2026-07-11T01:23:45Z"); };
    PlayerProfileManager manager(temp.path(), std::move(dependencies));
    expect(manager.Initialize().ok(),
           std::string(label) + " fixture seed initializes");
    const auto source = manager.createProfile(std::string(sourceName));
    expect(source.ok() && source.profile && source.profile->id == sourceId,
           std::string(label) + " fixture seed creates source profile");
    if (!source.profile) {
      return;
    }
    std::optional<ProfileResult> target;
    if (targetName) {
      target = manager.createProfile(std::string(*targetName));
      expect(target->ok() && target->profile && target->profile->id == targetId,
             std::string(label) + " fixture seed creates target profile");
      if (!target->profile) {
        return;
      }
    }
    expect(uuidIndex == uuids.size(),
           std::string(label) + " fixture seed consumes setup UUIDs");
    Fixture::seedMarker(manager.pathsFor(sourceId).scoresDb, sourceMarker);
    if (targetName) {
      Fixture::seedMarker(manager.pathsFor(targetId).scoresDb, targetMarker);
    }
    archive = exchange.path() / "source.asobprofile";
    ProfileArchiveService service(manager);
    const auto exported = service.Export(sourceId, archive);
    expect(exported.ok(), std::string(label) + " fixture seed exports source: " +
                              exported.message);
  }
};

const FaultArchiveFixtureSeed &rollbackFaultFixtureSeed() {
  static const FaultArchiveFixtureSeed seed{
      "profile-archive-rollback", "Rollback Source", "new-score",
      "Rollback Target", "old-score"};
  return seed;
}

const FaultArchiveFixtureSeed &overwriteFaultFixtureSeed() {
  static const FaultArchiveFixtureSeed seed{
      "profile-overwrite-fault", "Fault Source", "new-data", "Fault Target",
      "old-data"};
  return seed;
}

const FaultArchiveFixtureSeed &createFaultFixtureSeed() {
  static const FaultArchiveFixtureSeed seed{
      "profile-create-fault", "Create Fault Source", "imported-data"};
  return seed;
}

const FaultArchiveFixtureSeed &cleanupFaultFixtureSeed() {
  static const FaultArchiveFixtureSeed seed{
      "profile-overwrite-cleanup", "Cleanup Source", "new-data",
      "Cleanup Target", "old-data"};
  return seed;
}

bool copyFaultArchiveFixture(const FaultArchiveFixtureSeed &seed,
                             const std::filesystem::path &destination,
                             const std::filesystem::path &archive) {
  if (!Fixture::copyProfileTree(seed.temp.path(), destination)) {
    return false;
  }
  std::error_code error;
  std::filesystem::copy_file(seed.archive, archive,
                             std::filesystem::copy_options::none, error);
  if (error) {
    expect(false, "fault fixture archive copies: " + error.message());
    return false;
  }
  return true;
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

result_persistence::ModernChartResult portableReplayResult(int suffix,
                                                           char hash) {
  result_persistence::ModernChartResult value;
  value.attemptId =
      "123e4567-e89b-42d3-a456-42661417400" + std::to_string(suffix);
  value.score.chartPath = "portable/chart.bms";
  value.score.chartMd5 = repeated(hash, 32);
  value.score.chartSha256 = repeated(hash, 64);
  value.score.chartTitle = "Portable Replay";
  value.score.chartArtist = "Archive Contract";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = 4;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = 82.5F;
  value.score.clearType = kClearTypeNormalClearRank;
  value.score.provenance = ScoreProvenance::Legacy();
  value.keyMode = 7;
  value.adoptedGaugeType = GaugeType::Normal;
  value.adoptedGaugeHistory = {20.0F, 82.5F};
  value.playedAtUnixMillis = 1'700'000'000'000LL + suffix;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  return value;
}

ModernReplayFileReference installPortableReplay(const PlayerProfilePaths &paths,
                                                ReplayRepository &repository,
                                                int suffix, char hash,
                                                bool userDeleted = false) {
  const auto result = portableReplayResult(suffix, hash);
  const auto reserved = repository.ReserveModernReplayPath(
      result.attemptId, result.score.chartSha256, result.playedAtUnixMillis);
  expect(reserved.status == ModernReplayReservationStatus::Reserved &&
             reserved.reservation,
         "portable replay path reserves");
  const std::string payload = "portable replay " + std::to_string(suffix);
  const auto raw = std::as_bytes(std::span(payload.data(), payload.size()));
  const std::vector<std::byte> bytes(raw.begin(), raw.end());
  replay::ReplayFileStore store(paths.root);
  const auto fileReservation =
      store.reserve(reserved.reservation->identity, bytes, result.attemptId);
  const auto installed = store.install(*fileReservation.reservation, bytes);
  expect(installed.file.has_value(), "portable replay file installs");
  const ModernReplayFileAttachment attachment{
      .identity = reserved.reservation->identity,
      .metadata = installed.file->metadata};
  expect(repository.StageModernChartResult(result, std::nullopt, attachment, {})
                 .status == ModernChartStageStatus::Staged,
         "portable modern result stages");
  const auto loaded =
      repository.LoadModernChartResultByAttempt(result.attemptId);
  ModernReplayFileReference reference = *loaded.record->replayFile;
  if (userDeleted) {
    expect(repository
                   .MarkModernReplayFileUserDeleted(
                       ModernReplayOwnerKind::ChartResult, result.attemptId,
                       reference)
                   .status == ModernReplayFileMutationStatus::Changed,
           "portable replay deletion state persists");
    reference.userDeleted = true;
  }
  return reference;
}

void testStreamingSha256() {
  expect(file_checksum::sha256("") == "e3b0c44298fc1c149afbf4c8996fb924"
                                      "27ae41e4649b934ca495991b7852b855",
         "SHA-256 empty vector matches");
  expect(file_checksum::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223"
                                         "b00361a396177a9cb410ff61f20015ad",
         "SHA-256 abc vector matches");
  expect(file_checksum::sha256(
             "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
             "248d6a61d20638b8e5c026930c3e6039"
             "a33ce45964ff2167f6ecedd419db06c1",
         "SHA-256 multi-block vector matches");

  file_checksum::Sha256 stream;
  stream.update(std::as_bytes(std::span{"a", std::size_t{1}}));
  stream.update(std::as_bytes(std::span{"bc", std::size_t{2}}));
  expect(stream.finalHex() == file_checksum::sha256("abc"),
         "streamed SHA-256 equals single-buffer digest");

  TempDirectory temp{"checksum-limit"};
  const auto file = temp.path() / "bounded.bin";
  writeFile(file, "abc");
  std::string error;
  expect(!file_checksum::sha256File(file, error, 2) && !error.empty(),
         "streaming file checksum enforces its byte bound");
}

void testFixtureSeedIsBuiltOnceAndClonesAreIsolated() {
  Fixture first;
  Fixture second;

  expect(Fixture::seedBuildCountForTesting() == 1,
         "profile archive fixture seed builds exactly once");
  expect(first.uuidIndex == 3 && second.uuidIndex == 3,
         "profile archive fixture clones preserve the setup UUID cursor");
  const auto firstSettings =
      first.manager.pathsFor(first.sourceId).settingsJson;
  const auto secondSettings =
      second.manager.pathsFor(second.sourceId).settingsJson;
  writeFile(firstSettings, "{\"modified\":true}\n");
  expect(readFile(firstSettings).find("\"modified\":true") !=
             std::string::npos,
         "profile archive fixture mutation writes to the first clone");
  expect(readFile(secondSettings).find("\"modified\":true") ==
             std::string::npos,
         "profile archive fixture copies remain isolated");

  const auto firstArchive =
      Fixture::copyBaselineArchive(first, "baseline-first.asobprofile");
  const auto secondArchive =
      Fixture::copyBaselineArchive(second, "baseline-second.asobprofile");
  expect(Fixture::baselineArchiveBuildCountForTesting() == 1,
         "profile archive baseline export builds exactly once");
  writeFile(firstArchive, "modified baseline archive");
  expect(readFile(secondArchive) != "modified baseline archive",
         "profile archive baseline copies remain isolated");
}

std::filesystem::path exportFixture(Fixture &fixture, std::string_view name) {
  const auto archive = fixture.exchange.path() / name;
  ProfileArchiveService service(fixture.manager);
  const auto result = service.Export(fixture.sourceId, archive);
  expect(result.ok(), "profile export succeeds: " + result.message);
  return archive;
}

void expectRejectedWithoutMutation(
    Fixture &fixture, const std::vector<ArchiveMember> &members,
    std::string_view label,
    ProfileError expected = ProfileError::IntegrityFailure,
    std::string_view messageNeedle = {});

void testExportIsDeterministicAndStrict() {
  Fixture fixture;
  const auto first = exportFixture(fixture, "first.asobprofile");
  const auto second = exportFixture(fixture, "second.asobprofile");
  expect(readFile(first) == readFile(second),
         "equivalent exports are byte-for-byte deterministic");

  std::string error;
  const auto members = readArchive(first, error);
  expect(error.empty(), "exported ZIP reads: " + error);
  expect(members.size() == kExpectedMembers.size(),
         "export contains exactly seven members");
  for (std::size_t index = 0;
       index < std::min(members.size(), kExpectedMembers.size()); ++index) {
    expect(members[index].name == kExpectedMembers[index],
           "export member ordering is canonical");
    expect(members[index].type == AE_IFREG && !members[index].symlink &&
               !members[index].hardlink,
           "export members are plain regular files");
    expect(members[index].mtime == 0,
           "export member timestamps are deterministic");
  }
  const auto checksums = std::ranges::find(
      members, std::string("checksums.sha256"), &ArchiveMember::name);
  expect(checksums != members.end() &&
             checksums->contents == canonicalChecksums(members),
         "export checksum manifest is canonical and complete");

  const auto manifest = std::ranges::find(members, std::string("manifest.json"),
                                          &ArchiveMember::name);
  expect(manifest != members.end(), "export manifest is present");
  if (manifest != members.end()) {
    const Json document = Json::parse(manifest->contents, nullptr, false);
    expect(!document.is_discarded() && document.at("formatVersion") == 3 &&
               document.at("practiceSchemaVersion") == 1 &&
               document.at("profileUuid") == fixture.sourceId &&
               document.at("profileDisplayName") == "Portable Profile" &&
               document.at("scoreSchemaVersion") ==
                   ScoreRepository::kCurrentSchemaVersion &&
               document.at("replaySchemaVersion") ==
                   ReplayRepository::kCurrentSchemaVersion,
           "export manifest records portable version metadata");
  }
}

void testReplayFilesRoundTripByVerifiedOwnership() {
  Fixture fixture;
  const auto source = fixture.manager.pathsFor(fixture.sourceId);
  ReplayRepository repository(source.replaysDb);
  expect(repository.EnsureSchema(), "portable replay repository opens");
  const auto active = installPortableReplay(source, repository, 1, 'a');
  const auto missing = installPortableReplay(source, repository, 2, 'b');
  std::filesystem::remove(source.root / missing.metadata.relativePath);
  const auto deleted = installPortableReplay(source, repository, 3, 'c', true);
  auto future = installPortableReplay(source, repository, 4, 'd');
  repository.Shutdown();
  future.metadata.codecVersion =
      replay::BeatorajaReplayCodec::kCodecVersion + 1;
  setReplayCodecVersion(source.replaysDb, future.id,
                        future.metadata.codecVersion);
  writeFile(source.replayDirectory / "unreferenced.brd", "unreferenced");

  const auto archive = exportFixture(fixture, "owned-replays.asobprofile");
  std::string error;
  auto members = readArchive(archive, error);
  const std::string activeName = active.metadata.relativePath.generic_string();
  const std::string missingName =
      missing.metadata.relativePath.generic_string();
  const std::string deletedName =
      deleted.metadata.relativePath.generic_string();
  const std::string futureName = future.metadata.relativePath.generic_string();
  expect(error.empty() && findMember(members, activeName) != nullptr &&
             findMember(members, missingName) == nullptr &&
             findMember(members, deletedName) == nullptr &&
             findMember(members, futureName) == nullptr &&
             findMember(members, "replay/unreferenced.brd") == nullptr,
         "archive contains only current-codec verified replay bytes");
  const auto *checksums = findMember(members, "checksums.sha256");
  expect(checksums != nullptr &&
             checksums->contents.find("replay/") == std::string::npos &&
             checksums->contents.size() <=
                 ProfileArchiveService::kMaximumMetadataBytes,
         "replay inventory size does not grow the bounded checksum manifest");
  expect(std::filesystem::exists(source.replayDirectory / "unreferenced.brd"),
         "export leaves unreferenced source bytes untouched");

  ProfileArchiveService service(fixture.manager);
  auto legacyChecksumMembers = members;
  findMember(legacyChecksumMembers, "checksums.sha256")->contents =
      canonicalChecksums(legacyChecksumMembers, true);
  const auto legacyChecksumArchive =
      fixture.exchange.path() / "legacy-replay-checksums.asobprofile";
  expect(writeArchive(legacyChecksumArchive, legacyChecksumMembers, error),
         "legacy replay-checksum archive writes: " + error);
  const auto legacyChecksumImport = service.Import(legacyChecksumArchive);
  expect(legacyChecksumImport.ok(),
         "existing archives with replay checksum lines remain compatible: " +
             legacyChecksumImport.message);

  auto corruptReplayMembers = members;
  auto *corruptReplay = findMember(corruptReplayMembers, activeName);
  expect(corruptReplay != nullptr, "owned replay is available for corruption");
  if (corruptReplay != nullptr) {
    corruptReplay->contents = "corrupt replay bytes";
    refreshChecksums(corruptReplayMembers);
    expectRejectedWithoutMutation(
        fixture, corruptReplayMembers, "corrupt-owned-replay-member",
        ProfileError::IntegrityFailure, "Replay file");
  }

  const auto imported = service.Import(archive);
  expect(imported.ok() && imported.profile,
         "owned replay archive imports: " + imported.message);
  if (imported.profile) {
    const auto installed = fixture.manager.pathsFor(imported.profile->id);
    replay::ReplayFileStore installedStore(installed.root);
    expect(
        installedStore.inspect(active.metadata).state ==
                replay::ReplayFileState::Available &&
            !std::filesystem::exists(installed.root /
                                     missing.metadata.relativePath) &&
            !std::filesystem::exists(installed.root /
                                     deleted.metadata.relativePath) &&
            !std::filesystem::exists(installed.root /
                                     future.metadata.relativePath),
        "import restores verified bytes while preserving optional omissions");
    ReplayRepository installedRepository(installed.replaysDb);
    expect(installedRepository.EnsureSchema(),
           "imported replay repository opens");
    const auto inventory = installedRepository.ListModernReplayFileReferences();
    expect(inventory.status == ModernReplayFileInventoryStatus::Loaded &&
               inventory.entries.size() == 4,
           "import preserves result references independently of replay bytes");
  }

  ArchiveMember deletedMember{
      .name = deletedName,
      .contents = readFile(source.root / deleted.metadata.relativePath)};
  auto insertion =
      std::ranges::find_if(members, [&](const ArchiveMember &item) {
        return item.name == "checksums.sha256" ||
               (item.name.starts_with("replay/") && item.name > deletedName);
      });
  members.insert(insertion, std::move(deletedMember));
  refreshChecksums(members);
  expectRejectedWithoutMutation(fixture, members, "user-deleted-replay-member");

  writeFile(source.root / active.metadata.relativePath, "corrupt");
  const auto destination = fixture.exchange.path() / "corrupt-replay.zip";
  writeFile(destination, "existing archive");
  const auto corruptExport = service.Export(fixture.sourceId, destination);
  expect(corruptExport.error == ProfileError::IntegrityFailure &&
             readFile(destination) == "existing archive",
         "corrupt owned replay aborts export without replacing destination");
}

void testIrOperationalStateIsNotProfilePortable() {
  Fixture fixture;
  constexpr std::string_view credential = "sentinel-portable-api-key";
  const PlayerProfilePaths source = fixture.manager.pathsFor(fixture.sourceId);
  writeFile(
      source.irCredentialsJson,
      R"({"schemaVersion":1,"providers":{"tachi":{"apiKey":"sentinel-portable-api-key"}}})");
  constexpr std::string_view cacheMarker = "sentinel-bokutachi-cache-chart";
  writeFile(
      source.bokutachiCacheJson,
      R"({"schemaVersion":1,"origins":[{"serverOrigin":"https://boku.tachi.ac","userID":42,"charts":[{"game":"bms-7k","sha256":"abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd","chartID":"sentinel-bokutachi-cache-chart"}]}]})");
  seedIrOperationalState(source.replaysDb, "archive source");
  seedImportedIrScore(source.scoresDb, "archive source");

  const auto exported =
      exportFixture(fixture, "ir-operational-state.asobprofile");
  expect(rowCount(source.replaysDb, "ir_outbox") == 3,
         "export leaves source IR operational rows unchanged");
  expect(rowCount(source.replaysDb, "ir_submission_receipts") == 1 &&
             rowCount(source.replaysDb, "ir_remote_scores") == 1 &&
             matchingRowCount(source.scoresDb,
                              "SELECT COUNT(*) FROM scores WHERE "
                              "score_source=1") == 1,
         "export leaves source account-scoped IR data unchanged");
  expect(readFile(source.irCredentialsJson).find(credential) !=
             std::string::npos,
         "export leaves source credential bytes unchanged");

  std::string error;
  auto members = readArchive(exported, error);
  expect(error.empty(), "IR-safe export reads: " + error);
  expect(std::ranges::none_of(members,
                              [](const ArchiveMember &member) {
                                return member.name == "ir-credentials.json";
                              }),
         "credential file is absent from archive members and accounting");
  expect(std::ranges::none_of(members,
                              [](const ArchiveMember &member) {
                                return member.name == "bokutachi-cache.json";
                              }),
         "Bokutachi cache is absent from archive members and accounting");
  expect(std::ranges::none_of(members,
                              [&](const ArchiveMember &member) {
                                return member.contents.find(credential) !=
                                       std::string::npos;
                              }),
         "archive contains no credential bytes");
  expect(std::ranges::none_of(members,
                              [&](const ArchiveMember &member) {
                                return member.contents.find(cacheMarker) !=
                                       std::string::npos;
                              }),
         "archive contains no Bokutachi cache identifiers");

  ArchiveMember *replays = findMember(members, "replays.db");
  expect(replays != nullptr, "IR-safe export includes replay database");
  if (replays == nullptr) {
    return;
  }
  const auto portableDatabase = fixture.exchange.path() / "portable-replays.db";
  writeFile(portableDatabase, replays->contents);
  expect(rowCount(portableDatabase, "ir_outbox") == 0,
         "exported replay snapshot contains no IR operational rows");
  expect(rowCount(portableDatabase, "ir_submission_receipts") == 0 &&
             rowCount(portableDatabase, "ir_remote_scores") == 0,
         "exported replay snapshot contains no account-scoped IR data");

  ArchiveMember *scores = findMember(members, "scores.db");
  expect(scores != nullptr, "IR-safe export includes score database");
  if (scores == nullptr) {
    return;
  }
  const auto portableScores = fixture.exchange.path() / "portable-scores.db";
  writeFile(portableScores, scores->contents);
  expect(matchingRowCount(portableScores,
                          "SELECT COUNT(*) FROM scores WHERE score_source=1") ==
             0,
         "exported score snapshot contains no imported IR projections");

  seedIrOperationalState(portableDatabase, "crafted archive");
  seedImportedIrScore(portableScores, "crafted archive");
  replays->contents = readFile(portableDatabase);
  scores->contents = readFile(portableScores);
  refreshChecksums(members);
  const auto crafted = fixture.exchange.path() / "crafted-ir-state.zip";
  expect(writeArchive(crafted, members, error),
         "crafted compatible IR archive writes: " + error);
  ProfileArchiveService service(fixture.manager);
  const auto imported = service.Import(crafted);
  expect(imported.ok() && imported.profile,
         "crafted compatible IR archive imports: " + imported.message);
  if (!imported.profile) {
    return;
  }
  const PlayerProfilePaths installed =
      fixture.manager.pathsFor(imported.profile->id);
  expect(rowCount(installed.replaysDb, "ir_outbox") == 0,
         "import strips deliberately crafted IR operational rows");
  expect(rowCount(installed.replaysDb, "ir_submission_receipts") == 0 &&
             rowCount(installed.replaysDb, "ir_remote_scores") == 0 &&
             matchingRowCount(installed.scoresDb,
                              "SELECT COUNT(*) FROM scores WHERE "
                              "score_source=1") == 0,
         "import strips deliberately crafted account-scoped IR data");
  expect(!std::filesystem::exists(installed.irCredentialsJson),
         "imported profile has no credential file");
  expect(!std::filesystem::exists(installed.bokutachiCacheJson),
         "imported profile has no Bokutachi cache file");
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(installed.root)) {
    if (entry.is_regular_file()) {
      expect(readFile(entry.path()).find(credential) == std::string::npos,
             "imported profile file contains no credential bytes");
    }
  }
}

void testExportRejectsSupportedOlderSourceBeforeWritingArchive() {
  Fixture fixture;
  const PlayerProfilePaths source = fixture.manager.pathsFor(fixture.sourceId);
  setDatabaseVersion(source.scoresDb, 5, "supported-older export score");
  setDatabaseVersion(source.replaysDb, 3, "supported-older export replay");

  expect(fixture.manager.validateProfileForActivation(fixture.sourceId).ok(),
         "supported-older export source remains activatable");
  expect(fixture.manager.validateProfile(fixture.sourceId).error ==
             ProfileError::IntegrityFailure,
         "supported-older export source is not runtime ready");

  const auto destination =
      fixture.exchange.path() / "supported-older-export.asobprofile";
  bool temporaryArchiveWritten = false;
  ProfileArchiveDependencies dependencies;
  dependencies.beforeExportPhase = [&](ProfileArchiveExportPhase,
                                       std::string &) {
    temporaryArchiveWritten = true;
    return true;
  };
  ProfileArchiveService service(fixture.manager, std::move(dependencies));
  const auto exported = service.Export(fixture.sourceId, destination);
  expect(exported.error == ProfileError::IntegrityFailure &&
             !temporaryArchiveWritten && !std::filesystem::exists(destination),
         "RuntimeReady export rejects supported-older databases before "
         "writing an archive");

  std::string versionError;
  expect(sqliteDatabaseUserVersion(source.scoresDb, versionError) == 5,
         "rejected export preserves the older score version: " + versionError);
  versionError.clear();
  expect(sqliteDatabaseUserVersion(source.replaysDb, versionError) == 3 &&
             scalarText(source.scoresDb, "SELECT value FROM archive_marker") ==
                 "score-source" &&
             scalarText(source.replaysDb, "SELECT value FROM archive_marker") ==
                 "replay-source" &&
             transactionArtifacts(fixture.temp.path()).empty(),
         "rejected export neither migrates the source nor creates profile "
         "artifacts: " +
             versionError);
}

void testPresetStoreSidecarRemainsProfilePortable() {
  TempDirectory temp{"profile-practice-integration"};
  TempDirectory exchange{"profile-practice-integration-exchange"};
  std::vector<std::string> uuids{"11111111-1111-4111-8111-111111111111",
                                 "22222222-2222-4222-8222-222222222222"};
  std::size_t uuidIndex = 0;
  PlayerProfileManagerDependencies dependencies;
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  dependencies.utcNow = [] { return std::string("2026-07-11T02:34:56Z"); };
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(),
         "practice portability integration profile initializes");
  const std::string sourceId = manager.activeProfile().id;
  const PlayerProfilePaths source = manager.activePaths();

  practice::PresetStore store(source.practiceDirectory);
  practice::Configuration lastUsed{
      .chartSha256 = std::string(kPracticeHash),
      .startMicros = 1'000'000,
      .endMicros = 9'000'000,
      .loop = true,
      .gaugeType = GaugeType::ExHard,
      .gaugeAutoShift = GaugeAutoShiftMode::BestClear,
      .playback = {.percent = 90},
  };
  std::string error;
  expect(store.saveLastUsed(kPracticeHash, lastUsed, error),
         "integration last-used practice configuration saves: " + error);
  expect(store.saveNamed(kPracticeHash, "Opening", lastUsed, error).has_value(),
         "integration named practice preset saves: " + error);
  const auto primary =
      source.practiceDirectory / (std::string(kPracticeHash) + ".json");
  for (const std::string_view suffix :
       {".tmp", ".bak.pending", ".bak.previous"}) {
    writeFile(std::filesystem::path(primary.string() + std::string(suffix)),
              "bounded crash sidecar\n");
  }

  std::vector<std::string> sourceEntries;
  for (const auto &entry :
       std::filesystem::directory_iterator(source.practiceDirectory)) {
    sourceEntries.push_back(entry.path().filename().string());
  }
  std::ranges::sort(sourceEntries);
  expect(sourceEntries ==
             std::vector<std::string>{
                 std::string(kPracticeHash) + ".json",
                 std::string(kPracticeHash) + ".json.bak",
                 std::string(kPracticeHash) + ".json.bak.pending",
                 std::string(kPracticeHash) + ".json.bak.previous",
                 std::string(kPracticeHash) + ".json.tmp"},
         "live profile practice storage recognizes every exact atomic "
         "writer sidecar");
  expect(manager.validateProfile(sourceId).ok(),
         "profile validation accepts the PresetStore atomic backup sidecar");

  const auto duplicated = manager.duplicateProfile(sourceId, "Practice Copy");
  expect(duplicated.ok() && duplicated.profile,
         "profile with repeated PresetStore saves duplicates");
  if (duplicated.profile) {
    const auto duplicatePractice =
        manager.pathsFor(duplicated.profile->id).practiceDirectory;
    std::vector<std::string> duplicateEntries;
    for (const auto &entry :
         std::filesystem::directory_iterator(duplicatePractice)) {
      duplicateEntries.push_back(entry.path().filename().string());
    }
    expect(duplicateEntries ==
               std::vector<std::string>{std::string(kPracticeHash) + ".json"},
           "profile duplication copies the portable primary JSON, not its "
           "rollback sidecar");
    practice::PresetStore duplicateStore(duplicatePractice);
    const auto duplicatePreset = duplicateStore.load(kPracticeHash, 10'000'000);
    expect(duplicatePreset.status == versioned_json::LoadStatus::Loaded &&
               duplicatePreset.data.lastUsed.gaugeType == GaugeType::ExHard &&
               duplicatePreset.data.lastUsed.gaugeAutoShift ==
                   GaugeAutoShiftMode::BestClear,
           "profile duplication preserves practice Best Clear configuration");
  }

  const auto archive = exchange.path() / "practice-sidecar.asobprofile";
  ProfileArchiveService service(manager);
  const auto exported = service.Export(sourceId, archive);
  expect(exported.ok(),
         "profile with PresetStore sidecar exports: " + exported.message);
  if (exported.ok()) {
    auto members = readArchive(archive, error);
    const auto *practiceMember =
        findMember(members, "practice/" + std::string(kPracticeHash) + ".json");
    expect(practiceMember != nullptr &&
               std::ranges::none_of(
                   members,
                   [](const ArchiveMember &member) {
                     return member.name.starts_with("practice/") &&
                            member.name != "practice/" +
                                               std::string(kPracticeHash) +
                                               ".json";
                   }),
           "portable archive includes only the primary practice JSON");
    const Json practiceDocument =
        practiceMember == nullptr
            ? Json()
            : Json::parse(practiceMember->contents, nullptr, false);
    expect(practiceDocument.is_object() &&
               practiceDocument.at("lastUsed").at("gaugeAutoShift") ==
                   gaugeAutoShiftModeValue(GaugeAutoShiftMode::BestClear),
           "portable archive preserves Best Clear in practice preset JSON");
  }
}

void testMalformedOptionalPracticeRemainsVisibleButCannotExport() {
  Fixture fixture;
  const auto primary =
      fixture.manager.pathsFor(fixture.sourceId).practiceDirectory /
      (std::string(kPracticeHash) + ".json");
  const std::string validContents = readFile(primary);
  writeFile(primary, "{not-json");

  expect(
      fixture.manager.validateProfile(fixture.sourceId).ok() &&
          fixture.manager.validateProfileForActivation(fixture.sourceId).ok(),
      "malformed optional practice data remains visible and activatable");
  const auto visible = fixture.manager.listProfiles();
  expect(std::ranges::find(visible, fixture.sourceId, &PlayerProfile::id) !=
             visible.end(),
         "malformed optional practice data does not hide its profile");

  const auto destination = fixture.exchange.path() / "malformed-practice.zip";
  bool temporaryArchiveWritten = false;
  ProfileArchiveDependencies dependencies;
  dependencies.beforeExportPhase = [&](ProfileArchiveExportPhase,
                                       std::string &) {
    temporaryArchiveWritten = true;
    return true;
  };
  ProfileArchiveService service(fixture.manager, std::move(dependencies));
  const auto exported = service.Export(fixture.sourceId, destination);
  expect(exported.error == ProfileError::IntegrityFailure &&
             !std::filesystem::exists(destination) && !temporaryArchiveWritten,
         "portable export rejects malformed primary practice JSON before "
         "writing an archive");

  Json invalid = Json::parse(validContents);
  invalid["lastUsed"]["future"] = true;
  writeFile(primary, invalid.dump());
  temporaryArchiveWritten = false;
  const auto invalidDestination =
      fixture.exchange.path() / "invalid-practice.zip";
  const auto invalidExport =
      service.Export(fixture.sourceId, invalidDestination);
  expect(invalidExport.error == ProfileError::IntegrityFailure &&
             !std::filesystem::exists(invalidDestination) &&
             !temporaryArchiveWritten,
         "portable export rejects invalid primary practice JSON before "
         "writing an archive");
}

void testArchiveUsesNativeUnicodeFilesystemPaths() {
  Fixture fixture;
  const auto archive =
      fixture.exchange.path() /
      std::filesystem::path(u8"portable-profile-프로필-🎵.asobprofile");
  ProfileArchiveService service(fixture.manager);
  const auto exported = service.Export(fixture.sourceId, archive);
  expect(exported.ok() && std::filesystem::is_regular_file(archive),
         "profile archive exports through a native Unicode filesystem path: " +
             exported.message);
  bool inspectedPrivateImportWorkspace = false;
  std::filesystem::path observedWorkspace;
  ProfileArchiveDependencies dependencies;
  dependencies.importWorkspaceCreated =
      [&](const std::filesystem::path &workspace) {
        observedWorkspace = workspace;
      };
  dependencies.validation.declaredSizeAllowed = [&](std::string_view name,
                                                    std::uint64_t currentMember,
                                                    std::uint64_t currentTotal,
                                                    std::uint64_t additional) {
    if (!observedWorkspace.empty() && !inspectedPrivateImportWorkspace) {
      inspectedPrivateImportWorkspace = true;
#ifndef _WIN32
      expect((std::filesystem::status(observedWorkspace).permissions() &
              std::filesystem::perms::mask) ==
                     std::filesystem::perms::owner_all &&
                 (std::filesystem::status(observedWorkspace / "extracted")
                      .permissions() &
                  std::filesystem::perms::mask) ==
                     std::filesystem::perms::owner_all,
             "import workspace and extraction directory use mode 0700");
#else
      expect(
          hasProtectedCurrentUserOnlyDacl(observedWorkspace) &&
              hasProtectedCurrentUserOnlyDacl(observedWorkspace / "extracted"),
          "import workspace and extraction directory have protected "
          "current-user-only DACLs");
#endif
    }
    return ProfileArchiveSizePolicy::additionAllowed(name, currentMember,
                                                     currentTotal, additional);
  };
  ProfileArchiveService importService(fixture.manager, std::move(dependencies));
  const auto imported = importService.Import(archive);
  expect(imported.ok() && imported.profile && inspectedPrivateImportWorkspace,
         "profile archive imports through a native Unicode filesystem path: " +
             imported.message);
  expect(!observedWorkspace.empty() &&
             !std::filesystem::exists(observedWorkspace),
         "Unicode import cleans its private workspace");
}

void testCreateImportUsesNewIdAndRoundTripsExactly() {
  Fixture fixture;
  const auto archive =
      Fixture::copyBaselineArchive(fixture, "round-trip.asobprofile");
  const auto sourcePaths = fixture.manager.pathsFor(fixture.sourceId);
  const std::string sourceSettings = readFile(sourcePaths.settingsJson);
  const std::string sourceInput = readFile(sourcePaths.inputJson);
  const std::size_t countBefore = fixture.manager.listProfiles().size();
  std::filesystem::path observedWorkspace;

  ProfileArchiveDependencies dependencies;
  dependencies.importWorkspaceCreated =
      [&](const std::filesystem::path &workspace) {
        observedWorkspace = workspace;
      };
  ProfileArchiveService service(fixture.manager, std::move(dependencies));
  const auto imported = service.Import(archive);
  expect(imported.ok() && imported.profile.has_value(),
         "default archive import succeeds: " + imported.message);
  if (!imported.profile) {
    return;
  }
  expect(imported.profile->id != fixture.sourceId &&
             imported.profile->id == "44444444-4444-4444-8444-444444444444",
         "default import always assigns a fresh UUID on collision");
  expect(imported.profile->displayName == "Portable Profile",
         "default import preserves display name");
  expect(fixture.manager.listProfiles().size() == countBefore + 1,
         "default import adds exactly one profile");
  const auto importedPaths = fixture.manager.pathsFor(imported.profile->id);
  expect(readFile(importedPaths.settingsJson) == sourceSettings &&
             readFile(importedPaths.inputJson) == sourceInput,
         "settings and input bytes round-trip exactly");
  expect(AppSettingsStore::Load(importedPaths.settingsJson)
                 .settings.selectedGameplayRuleset == "beatoraja",
         "ruleset selection round-trips through profile archives");
  const auto importedSettings =
      AppSettingsStore::Load(importedPaths.settingsJson);
  const auto sourceLoadedSettings =
      AppSettingsStore::Load(sourcePaths.settingsJson);
  expect(
      importedSettings.status == AppSettingsLoadStatus::Loaded &&
          sourceLoadedSettings.status == AppSettingsLoadStatus::Loaded &&
          importedSettings.settings.skin == sourceLoadedSettings.settings.skin,
      "selected skin configuration and viewport round-trip through archives");
  expect(readFile(importedPaths.practiceDirectory /
                  (std::string(kPracticeHash) + ".json")) ==
             readFile(sourcePaths.practiceDirectory /
                      (std::string(kPracticeHash) + ".json")),
         "practice preset bytes round-trip exactly");
  expect(rowCount(importedPaths.scoresDb, "archive_marker") == 1 &&
             rowCount(importedPaths.replaysDb, "archive_marker") == 1,
         "score and replay database data round-trip exactly");
  const std::string expectedProvenance =
      R"(73|0|{"schemaVersion":1,"ruleset":{"version":73},"stages":[],"eligibility":"eligible"})";
  expect(scalarText(importedPaths.scoresDb,
                    "SELECT ruleset_version || '|' || eligibility || '|' || "
                    "provenance_json FROM scores WHERE chart_path = "
                    "'portable.bms'") == expectedProvenance,
         "score provenance payload round-trips exactly");
  expect(scalarText(importedPaths.replaysDb,
                    "SELECT ruleset_version || '|' || eligibility || '|' || "
                    "provenance_json FROM legacy_chart_result_summaries WHERE "
                    "legacy_replay_id=1") == expectedProvenance &&
             scalarText(importedPaths.replaysDb,
                        "SELECT partial || '|' || course_name || '|' || "
                        "completed_charts || '|' || total_charts FROM "
                        "legacy_course_result_summaries WHERE "
                        "legacy_course_replay_id=2") ==
                 "1|Portable Course|1|2" &&
             matchingRowCount(
                 importedPaths.replaysDb,
                 "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                 "name IN('replays','replay_events','replay_touch_samples',"
                 "'replay_lane_cover_events','course_replays',"
                 "'course_replay_stages')") == 0,
         "summary-only chart/course history round-trips without playback rows");
  expect(transactionArtifacts(fixture.temp.path()).empty(),
         "successful create import leaves no transaction artifacts");
  expect(!observedWorkspace.empty() &&
             !std::filesystem::exists(observedWorkspace),
         "successful import cleans its private workspace outside profile root");
}

void testVersionOneArchiveImportsWithEmptyPracticeDirectory() {
  Fixture fixture;
  const auto exported =
      Fixture::copyBaselineArchive(fixture, "version-three.asobprofile");
  std::string error;
  auto members = readArchive(exported, error);
  expect(error.empty(), "v1 compatibility fixture reads: " + error);
  auto versionTwoMembers = members;
  ArchiveMember *versionTwoManifest =
      findMember(versionTwoMembers, "manifest.json");
  expect(versionTwoManifest != nullptr, "v2 compatibility manifest exists");
  if (versionTwoManifest) {
    Json versionTwo = Json::parse(versionTwoManifest->contents, nullptr, false);
    versionTwo["formatVersion"] = 2;
    versionTwoManifest->contents = versionTwo.dump(2) + "\n";
    refreshChecksums(versionTwoMembers);
    const auto versionTwoArchive =
        fixture.exchange.path() / "version-two.asobprofile";
    expect(writeArchive(versionTwoArchive, versionTwoMembers, error),
           "v2 compatibility archive writes: " + error);
    ProfileArchiveService service(fixture.manager);
    const auto imported = service.Import(versionTwoArchive);
    expect(imported.ok() && imported.profile,
           "version-two profile archive still imports: " + imported.message);
    if (imported.profile) {
      expect(
          std::filesystem::is_empty(
              fixture.manager.pathsFor(imported.profile->id).replayDirectory),
          "version-two archive imports with an empty replay directory");
    }
  }
  std::erase_if(members, [](const ArchiveMember &member) {
    return member.name.starts_with("practice/");
  });
  ArchiveMember *manifestMember = findMember(members, "manifest.json");
  expect(manifestMember != nullptr, "v1 compatibility manifest exists");
  if (!manifestMember) {
    return;
  }
  Json manifest = Json::parse(manifestMember->contents, nullptr, false);
  manifest["formatVersion"] = 1;
  manifest.erase("practiceSchemaVersion");
  manifestMember->contents = manifest.dump(2) + "\n";
  refreshChecksums(members);
  const auto legacy = fixture.exchange.path() / "version-one.asobprofile";
  expect(writeArchive(legacy, members, error),
         "v1 compatibility archive writes: " + error);

  ProfileArchiveService service(fixture.manager);
  const auto imported = service.Import(legacy);
  expect(imported.ok() && imported.profile,
         "version-one profile archive still imports: " + imported.message);
  if (imported.profile) {
    const auto practiceDirectory =
        fixture.manager.pathsFor(imported.profile->id).practiceDirectory;
    expect(std::filesystem::is_directory(practiceDirectory) &&
               std::filesystem::is_empty(practiceDirectory),
           "version-one archive imports with an empty practice directory");
  }
}

void testCreateImportRetriesUnsafeAndOccupiedGeneratedIds() {
  Fixture fixture;
  const auto archive =
      Fixture::copyBaselineArchive(fixture, "uuid-retry.asobprofile");
  const std::string stagingId = "44444444-4444-4444-8444-444444444444";
  const std::string backupId = "55555555-5555-4555-8555-555555555555";
  const std::string importedId = "66666666-6666-4666-8666-666666666666";
  const auto staging =
      fixture.temp.path() / "profiles" / (".staging-" + stagingId);
  const auto backup =
      fixture.temp.path() / "profiles" / (".backup-" + backupId);
  writeFile(staging / "sentinel.txt", "staging-collision");
  writeFile(backup / "sentinel.txt", "backup-collision");
  const auto outside = fixture.temp.path() / "outside-sentinel.txt";
  writeFile(outside, "outside-data");

  fixture.uuids.resize(8);
  fixture.uuids[3] = "../../outside-sentinel.txt";
  fixture.uuids[4] = fixture.sourceId;
  fixture.uuids[5] = stagingId;
  fixture.uuids[6] = backupId;
  fixture.uuids[7] = importedId;

  ProfileArchiveService service(fixture.manager);
  const auto imported = service.Import(archive);
  expect(imported.ok() && imported.profile &&
             imported.profile->id == importedId,
         "create import retries invalid, existing, staging, and backup UUID "
         "collisions");
  expect(readFile(outside) == "outside-data" &&
             readFile(staging / "sentinel.txt") == "staging-collision" &&
             readFile(backup / "sentinel.txt") == "backup-collision",
         "UUID retries neither escape profile confinement nor consume other "
         "transaction artifacts");
}

void testOverwriteIsRestrictedAndReplacesInactiveProfile() {
  Fixture fixture;
  const auto archive =
      Fixture::copyBaselineArchive(fixture, "overwrite.asobprofile");
  ProfileArchiveService service(fixture.manager);

  ProfileImportOptions options;
  options.mode = ProfileImportMode::Overwrite;
  options.overwriteProfileId = fixture.targetId;
  const auto imported = service.Import(archive, options);
  expect(imported.ok() && imported.profile &&
             imported.profile->id == fixture.targetId,
         "explicit overwrite replaces the requested inactive profile");
  const auto targetPaths = fixture.manager.pathsFor(fixture.targetId);
  expect(
      rowCount(targetPaths.scoresDb, "archive_marker") == 1 &&
          readFile(targetPaths.settingsJson) ==
              readFile(fixture.manager.pathsFor(fixture.sourceId).settingsJson),
      "overwrite installs the imported profile components");
  ir::PendingIrCredentialCleanup pending(fixture.temp.path());
  std::string resetDiagnostic;
  expect(pending.pendingOverwriteResets(resetDiagnostic) ==
             std::vector<std::string>{fixture.targetId},
         "committed overwrite carries a durable credential reset marker");

  options.overwriteProfileId = fixture.manager.activeProfile().id;
  const auto active = service.Import(archive, options);
  expect(active.error == ProfileError::ActiveProfileDeletion,
         "overwrite refuses the active profile");

  options.overwriteProfileId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
  const auto missing = service.Import(archive, options);
  expect(missing.error == ProfileError::NotFound,
         "overwrite refuses a missing profile");

  options.overwriteProfileId.reset();
  const auto unspecified = service.Import(archive, options);
  expect(!unspecified.ok(), "overwrite requires an explicit target ID");
  expect(transactionArtifacts(fixture.temp.path()).empty(),
         "refused overwrite leaves no transaction artifacts");

  Fixture invalidFixture;
  const auto invalidArchive =
      Fixture::copyBaselineArchive(invalidFixture,
                                   "invalid-overwrite-target.zip");
  writeFile(
      invalidFixture.manager.pathsFor(invalidFixture.targetId).settingsJson,
      "not json");
  ProfileArchiveService invalidService(invalidFixture.manager);
  options.overwriteProfileId = invalidFixture.targetId;
  const auto invalid = invalidService.Import(invalidArchive, options);
  expect(invalid.error == ProfileError::IntegrityFailure,
         "overwrite refuses an invalid existing profile");
  expect(transactionArtifacts(invalidFixture.temp.path()).empty(),
         "invalid overwrite target creates no transaction artifacts");
}

void testOverwriteAcceptsSupportedOlderTargetAndInstallsCurrentProfile() {
  Fixture fixture;
  const auto archive =
      Fixture::copyBaselineArchive(fixture,
                                   "supported-older-overwrite.asobprofile");
  const PlayerProfilePaths target = fixture.manager.pathsFor(fixture.targetId);
  setDatabaseVersion(target.scoresDb, 5, "supported-older overwrite score");
  setDatabaseVersion(target.replaysDb, 3, "supported-older overwrite replay");
  expect(fixture.manager.validateProfileForActivation(fixture.targetId).ok() &&
             fixture.manager.validateProfile(fixture.targetId).error ==
                 ProfileError::IntegrityFailure,
         "supported-older overwrite target is manageable but not runtime "
         "ready");

  std::filesystem::path observedWorkspace;
  ProfileArchiveDependencies dependencies;
  dependencies.importWorkspaceCreated =
      [&](const std::filesystem::path &workspace) {
        observedWorkspace = workspace;
      };
  ProfileArchiveService service(fixture.manager, std::move(dependencies));
  ProfileImportOptions options{.mode = ProfileImportMode::Overwrite,
                               .overwriteProfileId = fixture.targetId};
  const auto imported = service.Import(archive, options);
  expect(imported.ok() && imported.profile &&
             imported.profile->id == fixture.targetId &&
             imported.profile->displayName == "Portable Profile",
         "Manage admits a supported-older inactive overwrite target: " +
             imported.message);

  std::string versionError;
  expect(sqliteDatabaseUserVersion(target.scoresDb, versionError) ==
                 ScoreRepository::kCurrentSchemaVersion &&
             sqliteDatabaseUserVersion(target.replaysDb, versionError) ==
                 ReplayRepository::kCurrentSchemaVersion &&
             fixture.manager.validateProfile(fixture.targetId).ok(),
         "overwrite installs a current RuntimeReady target: " + versionError);
  expect(
      scalarText(target.scoresDb, "SELECT value FROM archive_marker") ==
              "score-source" &&
          scalarText(target.replaysDb, "SELECT value FROM archive_marker") ==
              "replay-source" &&
          readFile(target.settingsJson) ==
              readFile(fixture.manager.pathsFor(fixture.sourceId).settingsJson),
      "overwrite installs the imported database and settings payload");
  expect(!observedWorkspace.empty() &&
             !std::filesystem::exists(observedWorkspace) &&
             transactionArtifacts(fixture.temp.path()).empty(),
         "supported-older overwrite cleans workspace, staging, and backup "
         "artifacts");
}

void testOverwriteRejectsFutureTargetWithoutMutation() {
  for (const bool futureScoreDatabase : {true, false}) {
    Fixture fixture;
    const auto archive = Fixture::copyBaselineArchive(
        fixture, futureScoreDatabase ? "future-score-overwrite.asobprofile"
                                     : "future-replay-overwrite.asobprofile");
    const PlayerProfilePaths target =
        fixture.manager.pathsFor(fixture.targetId);
    const std::filesystem::path futureDatabase =
        futureScoreDatabase ? target.scoresDb : target.replaysDb;
    const int futureVersion = futureScoreDatabase
                                  ? ScoreRepository::kCurrentSchemaVersion + 1
                                  : ReplayRepository::kCurrentSchemaVersion + 1;
    setDatabaseVersion(futureDatabase, futureVersion,
                       futureScoreDatabase ? "future overwrite score"
                                           : "future overwrite replay");

    const std::string metadataBefore = readFile(target.profileJson);
    const std::string settingsBefore = readFile(target.settingsJson);
    const std::string inputBefore = readFile(target.inputJson);
    const std::string scoresBefore = readFile(target.scoresDb);
    const std::string replaysBefore = readFile(target.replaysDb);
    std::filesystem::path observedWorkspace;
    ProfileArchiveDependencies dependencies;
    dependencies.importWorkspaceCreated =
        [&](const std::filesystem::path &workspace) {
          observedWorkspace = workspace;
        };
    ProfileArchiveService service(fixture.manager, std::move(dependencies));
    ProfileImportOptions options{.mode = ProfileImportMode::Overwrite,
                                 .overwriteProfileId = fixture.targetId};
    const auto imported = service.Import(archive, options);
    const std::string label =
        futureScoreDatabase ? "future score" : "future replay";
    expect(imported.error == ProfileError::FutureVersion,
           label + " overwrite target fails closed");

    std::string versionError;
    expect(std::filesystem::is_directory(target.root) &&
               sqliteDatabaseUserVersion(futureDatabase, versionError) ==
                   futureVersion &&
               readFile(target.profileJson) == metadataBefore &&
               readFile(target.settingsJson) == settingsBefore &&
               readFile(target.inputJson) == inputBefore &&
               readFile(target.scoresDb) == scoresBefore &&
               readFile(target.replaysDb) == replaysBefore,
           label +
               " overwrite rejection preserves target versions, metadata, "
               "and component bytes: " +
               versionError);
    expect(
        scalarText(target.scoresDb, "SELECT value FROM archive_marker") ==
                "score-target" &&
            scalarText(target.replaysDb, "SELECT value FROM archive_marker") ==
                "replay-target" &&
            !observedWorkspace.empty() &&
            !std::filesystem::exists(observedWorkspace) &&
            transactionArtifacts(fixture.temp.path()).empty(),
        label + " overwrite rejection preserves target payload and cleans "
                "all transaction artifacts");
  }
}

void testOverwriteRefusesTheLastProfile() {
  TempDirectory temp{"profile-last-overwrite"};
  TempDirectory exchange{"profile-last-overwrite-exchange"};
  PlayerProfileManagerDependencies dependencies;
  dependencies.generateUuid = [] {
    return std::string("11111111-1111-4111-8111-111111111111");
  };
  dependencies.utcNow = [] { return std::string("2026-07-11T01:23:45Z"); };
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "last-overwrite fixture initializes");
  ProfileArchiveService service(manager);
  const auto archive = exchange.path() / "last.zip";
  expect(service.Export(manager.activeProfile().id, archive).ok(),
         "last-overwrite fixture exports");
  ProfileImportOptions options{.mode = ProfileImportMode::Overwrite,
                               .overwriteProfileId =
                                   manager.activeProfile().id};
  const auto result = service.Import(archive, options);
  expect(result.error == ProfileError::LastProfileDeletion,
         "overwrite refuses the last remaining profile");
  expect(manager.validateProfile(manager.activeProfile().id).ok() &&
             transactionArtifacts(temp.path()).empty(),
         "last-profile refusal preserves the active profile without artifacts");
}

void testOverwriteRollbackRestoresOriginalProfile() {
  TempDirectory temp{"profile-archive-rollback"};
  TempDirectory exchange{"profile-archive-rollback-exchange"};
  const std::string activeId = "11111111-1111-4111-8111-111111111111";
  const std::string sourceId = "22222222-2222-4222-8222-222222222222";
  const std::string targetId = "33333333-3333-4333-8333-333333333333";
  std::vector<std::string> uuids{activeId, sourceId, targetId};
  std::size_t uuidIndex = 0;
  bool failReplacement = false;

  PlayerProfileManagerDependencies dependencies;
  dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
  dependencies.utcNow = [] { return std::string("2026-07-11T01:23:45Z"); };
  dependencies.filesystem.durableRename = [&](const std::filesystem::path &from,
                                              const std::filesystem::path &to,
                                              std::string &error) {
    if (failReplacement && from.filename().string().starts_with(".staging-") &&
        to.filename() == targetId) {
      error = "injected replacement failure";
      return false;
    }
    return atomic_file::renameDurably(from, to, error);
  };
  const auto archive = exchange.path() / "rollback.asobprofile";
  expect(copyFaultArchiveFixture(rollbackFaultFixtureSeed(), temp.path(),
                                 archive),
         "rollback fixture seed copies");
  uuidIndex = 3;
  PlayerProfileManager manager(temp.path(), std::move(dependencies));
  expect(manager.Initialize().ok(), "rollback fixture initializes");

  ProfileArchiveService service(manager);
  const auto targetPaths = manager.pathsFor(targetId);
  const std::string metadataBefore = readFile(targetPaths.profileJson);
  const std::string settingsBefore = readFile(targetPaths.settingsJson);
  const std::string inputBefore = readFile(targetPaths.inputJson);
  const std::string scoresBefore = readFile(targetPaths.scoresDb);
  const std::string replaysBefore = readFile(targetPaths.replaysDb);

  failReplacement = true;
  ProfileImportOptions options{.mode = ProfileImportMode::Overwrite,
                               .overwriteProfileId = targetId};
  const auto result = service.Import(archive, options);
  expect(result.error == ProfileError::IoFailure,
         "injected replacement failure is reported");
  expect(readFile(targetPaths.profileJson) == metadataBefore &&
             readFile(targetPaths.settingsJson) == settingsBefore &&
             readFile(targetPaths.inputJson) == inputBefore &&
             readFile(targetPaths.scoresDb) == scoresBefore &&
             readFile(targetPaths.replaysDb) == replaysBefore,
         "failed overwrite restores the original profile byte-for-byte");
  expect(manager.validateProfile(targetId).ok(),
         "rolled-back overwrite target remains valid");
  expect(transactionArtifacts(temp.path()).empty(),
         "failed overwrite cleans staging and backup artifacts");
  ir::PendingIrCredentialCleanup pending(temp.path());
  std::string resetDiagnostic;
  expect(pending.pendingOverwriteResets(resetDiagnostic).empty(),
         "rolled-back overwrite does not mark the original profile for reset");
}

void expectRejectedWithoutMutation(Fixture &fixture,
                                   const std::vector<ArchiveMember> &members,
                                   std::string_view label,
                                   ProfileError expected,
                                   std::string_view messageNeedle) {
  const auto archive = fixture.temp.path() / (std::string(label) + ".zip");
  std::string error;
  expect(writeArchive(archive, members, error),
         std::string(label) + " malicious archive writes: " + error);
  const std::size_t profilesBefore = fixture.manager.listProfiles().size();
  const std::size_t uuidIndexBefore = fixture.uuidIndex;
  std::filesystem::path observedWorkspace;
  ProfileArchiveDependencies dependencies;
  dependencies.importWorkspaceCreated =
      [&](const std::filesystem::path &workspace) {
        observedWorkspace = workspace;
      };
  ProfileArchiveService service(fixture.manager, std::move(dependencies));
  const auto result = service.Import(archive);
  expect(result.error == expected &&
             (messageNeedle.empty() ||
              result.message.find(messageNeedle) != std::string::npos),
         std::string(label) + " archive is rejected: " + result.message);
  expect(fixture.manager.listProfiles().size() == profilesBefore,
         std::string(label) + " rejection does not create a profile");
  expect(fixture.uuidIndex == uuidIndexBefore,
         std::string(label) + " rejection occurs before profile install");
  expect(transactionArtifacts(fixture.temp.path()).empty(),
         std::string(label) + " rejection leaves no transaction artifacts");
  expect(!observedWorkspace.empty() &&
             !std::filesystem::exists(observedWorkspace),
         std::string(label) + " rejection cleans its private import workspace");
}

void testStrictMemberAllowlistAndTypes() {
  Fixture fixture;
  const auto validPath =
      Fixture::copyBaselineArchive(fixture, "strict-source.zip");
  std::string error;
  const auto valid = readArchive(validPath, error);
  expect(error.empty() && valid.size() == kExpectedMembers.size(),
         "strict source archive reads");
  if (valid.size() != kExpectedMembers.size()) {
    return;
  }

  auto missing = valid;
  missing.erase(missing.begin());
  expectRejectedWithoutMutation(fixture, missing, "missing-member");

  auto extra = valid;
  extra.push_back({.name = "extra.txt", .contents = "unexpected"});
  expectRejectedWithoutMutation(fixture, extra, "extra-member");

  auto credentials = valid;
  credentials.push_back(
      {.name = "ir-credentials.json", .contents = "sentinel-api-key"});
  expectRejectedWithoutMutation(
      fixture, credentials, "credential-member-is-unknown",
      ProfileError::IntegrityFailure, "unexpected member name");

  auto duplicate = valid;
  duplicate.push_back(valid.front());
  expectRejectedWithoutMutation(fixture, duplicate, "duplicate-member");

  for (const std::string hostileName : std::array{
           std::string("../manifest.json"), std::string("/manifest.json"),
           std::string("C:/manifest.json"), std::string("folder/manifest.json"),
           std::string("practice/not-a-hash.json"),
           std::string("practice/") + std::string(kPracticeHash) + ".json.tmp",
           std::string("practice/") + std::string(kPracticeHash) + ".json.bak",
           std::string("practice/") + std::string(kPracticeHash) + ".json.old",
           std::string("practice/"
                       "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                       "AAAAAAAAA.json"),
           std::string("bad-\xff.json", 10)}) {
    auto hostile = valid;
    hostile.front().name = hostileName;
    expectRejectedWithoutMutation(
        fixture, hostile, "hostile-name-" + std::to_string(hostileName.size()));
  }

  auto symlink = valid;
  symlink.front().type = AE_IFLNK;
  symlink.front().symlink = "settings.json";
  symlink.front().contents.clear();
  expectRejectedWithoutMutation(fixture, symlink, "symlink-member");

  auto hardlink = valid;
  hardlink.front().hardlink = "settings.json";
  hardlink.front().contents.clear();
  expectRejectedWithoutMutation(fixture, hardlink, "hardlink-member");

  auto directory = valid;
  directory.front().type = AE_IFDIR;
  directory.front().contents.clear();
  expectRejectedWithoutMutation(fixture, directory, "directory-member");
}

void testPracticeMembersAreValidatedBeforeInstall() {
  Fixture fixture;
  const auto validPath =
      Fixture::copyBaselineArchive(fixture, "practice-validation.zip");
  std::string error;
  auto valid = readArchive(validPath, error);
  expect(error.empty(), "practice validation source archive reads: " + error);
  const std::string memberName =
      "practice/" + std::string(kPracticeHash) + ".json";

  auto rejectPractice = [&](std::vector<ArchiveMember> members,
                            std::string contents, std::string_view label,
                            ProfileError expected =
                                ProfileError::IntegrityFailure) {
    ArchiveMember *practiceMember = findMember(members, memberName);
    expect(practiceMember != nullptr,
           std::string(label) + " source practice member exists");
    if (!practiceMember) {
      return;
    }
    practiceMember->contents = std::move(contents);
    refreshChecksums(members);
    expectRejectedWithoutMutation(fixture, members, label, expected);
  };

  rejectPractice(valid, "{not-json", "malformed-practice");

  ArchiveMember *validPractice = findMember(valid, memberName);
  if (!validPractice) {
    return;
  }
  Json document = Json::parse(validPractice->contents);
  Json future = document;
  future["schemaVersion"] = practice::kPresetSchemaVersion + 1;
  rejectPractice(valid, future.dump(), "future-practice",
                 ProfileError::FutureVersion);

  Json wrongSchema = document;
  wrongSchema["schemaVersion"] = practice::kPresetSchemaVersion - 1;
  rejectPractice(valid, wrongSchema.dump(), "mismatched-practice-schema");

  Json mismatched = document;
  mismatched["chartSha256"] =
      "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  rejectPractice(valid, mismatched.dump(), "mismatched-practice-hash");

  Json semantic = document;
  semantic["lastUsed"]["countInBeats"] = 17;
  rejectPractice(valid, semantic.dump(), "invalid-practice-semantics");

  Json undeclared = document;
  undeclared["lastUsed"]["judge"]["future"] = true;
  rejectPractice(valid, undeclared.dump(), "undeclared-practice-field");

  Json duplicateIds = document;
  Json namedEntry{{"id", "0123456789abcdef0123456789abcdef"},
                  {"name", "Opening"},
                  {"configuration", document["lastUsed"]}};
  duplicateIds["named"].push_back(namedEntry);
  duplicateIds["named"].push_back(namedEntry);
  rejectPractice(valid, duplicateIds.dump(), "duplicate-practice-preset-id");

  ProfileArchiveService service(fixture.manager);
  const auto imported = service.Import(validPath);
  expect(imported.ok() && imported.profile,
         "valid semantic practice data imports: " + imported.message);
  if (imported.profile) {
    const auto loaded =
        practice::PresetStore(
            fixture.manager.pathsFor(imported.profile->id).practiceDirectory)
            .load(kPracticeHash, 10'000'000);
    expect(loaded.status == versioned_json::LoadStatus::Loaded,
           "imported valid practice data remains readable");
  }
}

void testChecksumsVersionsValidatorsAndLimits() {
  Fixture fixture;
  const auto validPath =
      Fixture::copyBaselineArchive(fixture, "validation-source.zip");
  std::string error;
  const auto valid = readArchive(validPath, error);
  expect(error.empty() && valid.size() == kExpectedMembers.size(),
         "validation source archive reads");
  if (valid.size() != kExpectedMembers.size()) {
    return;
  }

  auto mismatch = valid;
  findMember(mismatch, "settings.json")->contents += " ";
  expectRejectedWithoutMutation(fixture, mismatch, "checksum-mismatch");

  auto malformedChecksums = valid;
  findMember(malformedChecksums, "checksums.sha256")->contents +=
      "00  manifest.json\n";
  expectRejectedWithoutMutation(fixture, malformedChecksums,
                                "malformed-checksum-list");

  auto futureFormat = valid;
  Json manifest =
      Json::parse(findMember(futureFormat, "manifest.json")->contents);
  manifest["formatVersion"] = ProfileArchiveManifest::kFormatVersion + 1;
  findMember(futureFormat, "manifest.json")->contents = manifest.dump(2) + "\n";
  refreshChecksums(futureFormat);
  expectRejectedWithoutMutation(fixture, futureFormat, "future-format",
                                ProfileError::FutureVersion);

  for (const auto &[field, current] :
       std::array<std::pair<std::string_view, int>, 6>{
           std::pair{"profileSchemaVersion", kPlayerProfileSchemaVersion},
           std::pair{"settingsSchemaVersion",
                     AppSettingsStore::kCurrentSchemaVersion},
           std::pair{"inputSchemaVersion", InputProfile::kSchemaVersion},
           std::pair{"practiceSchemaVersion", 1},
           std::pair{"scoreSchemaVersion",
                     ScoreRepository::kCurrentSchemaVersion},
           std::pair{"replaySchemaVersion",
                     ReplayRepository::kCurrentSchemaVersion}}) {
    auto futureComponent = valid;
    manifest =
        Json::parse(findMember(futureComponent, "manifest.json")->contents);
    manifest[std::string(field)] = current + 1;
    findMember(futureComponent, "manifest.json")->contents =
        manifest.dump(2) + "\n";
    refreshChecksums(futureComponent);
    expectRejectedWithoutMutation(fixture, futureComponent,
                                  "future-" + std::string(field),
                                  ProfileError::FutureVersion);
  }

  auto invalidSettings = valid;
  findMember(invalidSettings, "settings.json")->contents = "not json";
  refreshChecksums(invalidSettings);
  expectRejectedWithoutMutation(fixture, invalidSettings, "invalid-settings");

  auto futureSettings = valid;
  Json settings =
      Json::parse(findMember(futureSettings, "settings.json")->contents);
  settings["schemaVersion"] = AppSettingsStore::kCurrentSchemaVersion + 1;
  findMember(futureSettings, "settings.json")->contents =
      settings.dump(2) + "\n";
  refreshChecksums(futureSettings);
  expectRejectedWithoutMutation(fixture, futureSettings, "future-settings",
                                ProfileError::FutureVersion);

  auto invalidInput = valid;
  findMember(invalidInput, "input.json")->contents = "[]\n";
  refreshChecksums(invalidInput);
  expectRejectedWithoutMutation(fixture, invalidInput, "invalid-input");

  for (const std::string_view name :
       {"manifest.json", "settings.json", "input.json"}) {
    auto invalidUtf8 = valid;
    findMember(invalidUtf8, name)->contents.push_back(static_cast<char>(0xff));
    refreshChecksums(invalidUtf8);
    expectRejectedWithoutMutation(fixture, invalidUtf8,
                                  "invalid-utf8-" + std::string(name));
  }

  auto oversizedManifest = valid;
  findMember(oversizedManifest, "manifest.json")->contents =
      std::string(ProfileArchiveService::kMaximumMetadataBytes + 1, 'x');
  refreshChecksums(oversizedManifest);
  expectRejectedWithoutMutation(fixture, oversizedManifest,
                                "oversized-manifest");

  auto unsupportedLegacyScore = valid;
  manifest = Json::parse(
      findMember(unsupportedLegacyScore, "manifest.json")->contents);
  manifest["scoreSchemaVersion"] = 3;
  findMember(unsupportedLegacyScore, "manifest.json")->contents =
      manifest.dump(2) + "\n";
  refreshChecksums(unsupportedLegacyScore);
  const auto unsupportedPath = fixture.temp.path() / "unsupported-score-v3.zip";
  expect(writeArchive(unsupportedPath, unsupportedLegacyScore, error),
         "unsupported legacy score archive writes: " + error);
  ProfileArchiveService service(fixture.manager);
  const auto unsupported = service.Import(unsupportedPath);
  expect(unsupported.error == ProfileError::IntegrityFailure &&
             unsupported.message.find("older than version 4") !=
                 std::string::npos &&
             unsupported.message.find("chart library context") !=
                 std::string::npos,
         "score schemas before v4 fail closed with migration-context guidance");
}

void testSizePolicyBoundariesWithoutLargeAllocations() {
  using Policy = ProfileArchiveSizePolicy;
  expect(Policy::memberSizeAllowed("manifest.json",
                                   Policy::kMaximumMetadataBytes) &&
             !Policy::memberSizeAllowed("manifest.json",
                                        Policy::kMaximumMetadataBytes + 1),
         "metadata declared-size boundary is enforced");
  expect(
      Policy::memberSizeAllowed("scores.db", Policy::kMaximumDatabaseBytes) &&
          !Policy::memberSizeAllowed("scores.db",
                                     Policy::kMaximumDatabaseBytes + 1),
      "database declared-size boundary is enforced");
  std::string replayDiagnostic;
  const auto replayStem =
      replay::chartStem(std::string(64, 'a'), 1, false, replayDiagnostic);
  const auto replayPath = replay::pathForStem(*replayStem, 0, replayDiagnostic);
  const std::string replayMember = replayPath->relativePath.generic_string();
  expect(Policy::memberSizeAllowed(replayMember,
                                   Policy::kMaximumReplayFileBytes) &&
             !Policy::memberSizeAllowed(replayMember,
                                        Policy::kMaximumReplayFileBytes + 1),
         "replay member declared-size boundary shares the BRD file limit");
  expect(Policy::additionAllowed("scores.db", Policy::kMaximumDatabaseBytes, 0,
                                 0) &&
             !Policy::additionAllowed("scores.db",
                                      Policy::kMaximumDatabaseBytes, 0, 1),
         "per-stream database growth beyond 2 GiB is rejected");
  expect(
      Policy::additionAllowed("replays.db", 0, Policy::kMaximumTotalBytes, 0) &&
          !Policy::additionAllowed("replays.db", 0, Policy::kMaximumTotalBytes,
                                   1),
      "aggregate stream growth beyond 4 GiB is rejected");
  expect(Policy::totalSizeAllowed(Policy::kMaximumTotalBytes) &&
             !Policy::totalSizeAllowed(Policy::kMaximumTotalBytes + 1),
         "aggregate declared-size boundary is enforced");
}

void testZipParserEnforcesDeclaredAndStreamedSizeLimits() {
  Fixture fixture;
  const auto source =
      Fixture::copyBaselineArchive(fixture, "size-parser-source.zip");
  std::string error;
  const auto valid = readArchive(source, error);
  expect(error.empty() && valid.size() == kExpectedMembers.size(),
         "size parser source archive reads");
  if (!error.empty() || valid.size() != kExpectedMembers.size()) {
    return;
  }
  ProfileArchiveService service(fixture.manager);
  const std::size_t profileCount = fixture.manager.listProfiles().size();
  auto expectPatchedRejected = [&](const std::filesystem::path &archive,
                                   std::string_view expectedMessage,
                                   std::string_view label) {
    const auto result = service.Import(archive);
    expect(result.error == ProfileError::IntegrityFailure &&
               result.message.find(expectedMessage) != std::string::npos,
           std::string(label) +
               " is rejected by the full ZIP parser: " + result.message);
    expect(fixture.manager.listProfiles().size() == profileCount &&
               transactionArtifacts(fixture.temp.path()).empty(),
           std::string(label) + " rejection leaves profiles unchanged");
  };

  const auto oversizedDatabase =
      fixture.temp.path() / "declared-database-too-large.zip";
  expect(writeArchive(oversizedDatabase, valid, error) &&
             patchZipDeclaredSize(
                 oversizedDatabase, "scores.db",
                 static_cast<std::uint32_t>(
                     ProfileArchiveSizePolicy::kMaximumDatabaseBytes + 1)),
         "oversized declared database ZIP fixture patches");
  expectPatchedRejected(oversizedDatabase, "declared size",
                        "2 GiB declared database limit");

  std::uint64_t validDeclaredTotal = 0;
  for (const ArchiveMember &member : valid) {
    validDeclaredTotal += member.contents.size();
  }
  ProfileArchiveDependencies aggregateDependencies;
  aggregateDependencies.validation.declaredSizeAllowed =
      [maximumTotal = validDeclaredTotal -
                      1](std::string_view name, std::uint64_t currentMember,
                         std::uint64_t currentTotal, std::uint64_t additional) {
        return ProfileArchiveSizePolicy::memberSizeAllowed(
                   name, currentMember + additional) &&
               currentTotal <= maximumTotal &&
               additional <= maximumTotal - currentTotal;
      };
  ProfileArchiveService aggregateLimited(fixture.manager,
                                         aggregateDependencies);
  const auto aggregateResult = aggregateLimited.Import(source);
  expect(aggregateResult.error == ProfileError::IntegrityFailure &&
             aggregateResult.message.find("declared size") != std::string::npos,
         "injectable small aggregate limit exercises full declared-size "
         "parser enforcement");

  ProfileArchiveDependencies streamDependencies;
  streamDependencies.validation.streamedSizeAllowed =
      [](std::string_view, std::uint64_t currentMember,
         std::uint64_t currentTotal, std::uint64_t additional) {
        constexpr std::uint64_t kSmallStreamLimit = 1;
        return currentMember <= kSmallStreamLimit &&
               currentTotal <= kSmallStreamLimit &&
               additional <= kSmallStreamLimit - currentMember &&
               additional <= kSmallStreamLimit - currentTotal;
      };
  ProfileArchiveService streamLimited(fixture.manager, streamDependencies);
  const auto streamResult = streamLimited.Import(source);
  expect(streamResult.error == ProfileError::IntegrityFailure &&
             streamResult.message.find("stream exceeds") != std::string::npos,
         "injectable small stream limit exercises full extracted-size parser "
         "enforcement");
  expect(fixture.manager.listProfiles().size() == profileCount &&
             transactionArtifacts(fixture.temp.path()).empty(),
         "declared and streamed size parser failures leave profiles unchanged");
}

void testSupportedOlderSchemasMigrateAndPreserveRows() {
  Fixture fixture;
  const auto validPath =
      Fixture::copyBaselineArchive(fixture, "migration-source.zip");
  std::string error;
  auto members = readArchive(validPath, error);
  expect(error.empty() && members.size() == kExpectedMembers.size(),
         "migration source archive reads");
  if (!error.empty() || members.size() != kExpectedMembers.size()) {
    return;
  }

  Json manifest = Json::parse(findMember(members, "manifest.json")->contents);
  manifest["settingsSchemaVersion"] = 0;
  manifest["inputSchemaVersion"] = 0;
  manifest["scoreSchemaVersion"] = 4;
  manifest["replaySchemaVersion"] = 2;
  findMember(members, "manifest.json")->contents = manifest.dump(2) + "\n";

  Json settings = Json::parse(findMember(members, "settings.json")->contents);
  settings["schemaVersion"] = 0;
  findMember(members, "settings.json")->contents = settings.dump(2) + "\n";
  Json input = Json::parse(findMember(members, "input.json")->contents);
  input["schemaVersion"] = 0;
  findMember(members, "input.json")->contents = input.dump(2) + "\n";

  for (const auto &[name, version] :
       std::array<std::pair<std::string_view, int>, 2>{
           std::pair{"scores.db", 4}, std::pair{"replays.db", 2}}) {
    const auto databasePath =
        fixture.temp.path() / (std::string(name) + ".old");
    if (name == "scores.db") {
      writeFile(databasePath, findMember(members, name)->contents);
      Database database = openDatabase(databasePath);
      expect(database != nullptr, "older score database fixture opens");
      if (!database) {
        return;
      }
      expect(execute(database.get(), "DROP TRIGGER IF EXISTS "
                                     "score_sha256_summary_after_insert"),
             "legacy score fixture removes the current summary trigger");
      for (const std::string_view table : {"scores", "course_scores"}) {
        for (const std::string_view column :
             {"provenance_json", "eligibility", "ruleset_version"}) {
          expect(execute(database.get(), "ALTER TABLE " + std::string(table) +
                                             " DROP COLUMN " +
                                             std::string(column)),
                 "authentic legacy fixture removes " + std::string(table) +
                     "." + std::string(column));
        }
      }
      expect(execute(database.get(),
                     "PRAGMA user_version = " + std::to_string(version)),
             "older score database fixture version updates");
      database.reset();
    } else {
      std::string replayFixtureError;
      expect(
          replay_legacy_fixture::replaceWithVersion2Database(
              databasePath, version,
              "INSERT INTO replays(id,chart_path,chart_md5,chart_sha256,"
              "chart_title,chart_artist,ln_mode,gauge_type,gauge_auto_shift,"
              "final_score,max_combo,final_gauge,clear_type) VALUES(1,"
              "'portable.bms','0123456789abcdef0123456789abcdef',"
              "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
              "a',"
              "'Portable Song','Portable Artist',0,0,0,123,7,0.75,2);"
              "INSERT INTO replay_events(replay_id,event_index,action,lane,"
              "note_time_micros,song_time_micros,judge_time_micros,judgement,"
              "diff_micros,gauge,gauge_type,combo,score) VALUES"
              "(1,0,1,2,1000,1001,1002,0,2,0.75,0,999,999999);"
              "INSERT INTO course_replays(id,course_id,course_name,"
              "course_group_name,constraint_json,gauge_type,gauge_profile,"
              "gauge_auto_shift,ln_mode,final_score,max_combo,final_gauge,"
              "clear_type,completed_charts,total_charts) VALUES"
              "(2,7,'Portable Course','Archive Contract','[]',0,0,0,0,"
              "321,8,0.5,2,1,2);"
              "INSERT INTO course_replay_stages(course_replay_id,"
              "stage_index,replay_id,rest_micros_after_stage) VALUES"
              "(2,0,1,123456)",
              replayFixtureError),
          "authentic legacy replay fixture is written: " + replayFixtureError);
    }
    findMember(members, name)->contents = readFile(databasePath);
  }
  refreshChecksums(members);
  const auto archive = fixture.temp.path() / "supported-older.zip";
  expect(writeArchive(archive, members, error),
         "supported older archive writes: " + error);

  ProfileArchiveService service(fixture.manager);
  const auto imported = service.Import(archive);
  expect(imported.ok() && imported.profile,
         "supported older schemas import through migrations: " +
             imported.message);
  if (!imported.profile) {
    return;
  }
  const auto paths = fixture.manager.pathsFor(imported.profile->id);
  const Json migratedSettings = Json::parse(readFile(paths.settingsJson));
  const Json migratedInput = Json::parse(readFile(paths.inputJson));
  std::string versionError;
  expect(migratedSettings.at("schemaVersion") ==
                 AppSettingsStore::kCurrentSchemaVersion &&
             migratedInput.at("schemaVersion") == InputProfile::kSchemaVersion,
         "older settings and input documents persist at current schemas");
  expect(sqliteDatabaseUserVersion(paths.scoresDb, versionError) ==
                 ScoreRepository::kCurrentSchemaVersion &&
             sqliteDatabaseUserVersion(paths.replaysDb, versionError) ==
                 ReplayRepository::kCurrentSchemaVersion,
         "older score and replay databases migrate to current schemas");
  expect(rowCount(paths.scoresDb, "scores") == 1 &&
             rowCount(paths.replaysDb, "legacy_chart_result_summaries") == 1 &&
             rowCount(paths.replaysDb, "legacy_course_result_summaries") == 1,
         "database migrations preserve score and legacy header summaries");
  const std::string legacyProvenance =
      R"({"schemaVersion":1,"ruleset":{"version":0},"stages":[],"eligibility":"legacy-unverified"})";
  expect(scalarText(paths.scoresDb,
                    "SELECT chart_title || '|' || chart_artist || '|' || "
                    "score || '|' || max_score || '|' || max_combo || '|' || "
                    "ruleset_version || '|' || eligibility || '|' || "
                    "provenance_json FROM scores WHERE chart_path = "
                    "'portable.bms'") ==
             "Portable Song|Portable Artist|123|456|7|0|2|" + legacyProvenance,
         "score v4 migration preserves payload and backfills provenance");
  expect(scalarText(
             paths.replaysDb,
             "SELECT chart_title || '|' || chart_artist || '|' || "
             "final_score || '|' || max_combo || '|' || "
             "(ruleset_version IS NULL) || '|' || (eligibility IS NULL) || "
             "'|' || (provenance_json IS NULL) || '|' || partial FROM "
             "legacy_chart_result_summaries WHERE chart_path='portable.bms'") ==
             "Portable Song|Portable Artist|123|7|1|1|1|1",
         "replay v2 migration preserves chart headers and marks unavailable "
         "provenance partial");
  expect(scalarText(paths.replaysDb,
                    "SELECT course_name || '|' || final_score || '|' || "
                    "completed_charts || '|' || total_charts FROM "
                    "legacy_course_result_summaries WHERE "
                    "legacy_course_replay_id=2") == "Portable Course|321|1|2",
         "replay v2 migration preserves course headers");
  expect(matchingRowCount(
             paths.replaysDb,
             "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
             "name IN('replays','replay_events','replay_touch_samples',"
             "'replay_lane_cover_events','course_replays',"
             "'course_replay_stages')") == 0,
         "replay v2 migration drops all playback tables");
}

void testFutureDatabaseAndCorruptionAreRejected() {
  Fixture fixture;
  const auto validPath =
      Fixture::copyBaselineArchive(fixture, "database-source.zip");
  std::string error;
  const auto valid = readArchive(validPath, error);
  if (!error.empty() || valid.size() != kExpectedMembers.size()) {
    expect(false, "database source archive reads: " + error);
    return;
  }

  auto future = valid;
  const auto temporaryDb = fixture.temp.path() / "future-scores.db";
  writeFile(temporaryDb, findMember(future, "scores.db")->contents);
  Database database = openDatabase(temporaryDb);
  expect(database != nullptr &&
             execute(database.get(),
                     "PRAGMA user_version = " +
                         std::to_string(ScoreRepository::kCurrentSchemaVersion +
                                        1)),
         "future database fixture updates user_version");
  database.reset();
  findMember(future, "scores.db")->contents = readFile(temporaryDb);
  refreshChecksums(future);
  expectRejectedWithoutMutation(fixture, future, "future-database",
                                ProfileError::FutureVersion);

  auto corrupt = valid;
  findMember(corrupt, "replays.db")->contents = "not sqlite";
  refreshChecksums(corrupt);
  expectRejectedWithoutMutation(fixture, corrupt, "corrupt-database");
}

void testExportFailurePreservesDestinationAndCleansTemps() {
  Fixture fixture;
  const auto destination = fixture.exchange.path() / "existing.asobprofile";
  writeFile(destination, "existing-destination");
  ProfileArchiveService service(fixture.manager);
  const auto result =
      service.Export("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", destination);
  expect(result.error == ProfileError::NotFound,
         "export rejects a missing source profile");
  expect(readFile(destination) == "existing-destination",
         "failed export preserves a pre-existing destination");
  std::size_t siblingTemps = 0;
  for (const auto &entry :
       std::filesystem::directory_iterator(fixture.exchange.path())) {
    if (entry.path().filename().string().starts_with(
            "existing.asobprofile.tmp")) {
      ++siblingTemps;
    }
  }
  expect(siblingTemps == 0, "failed export cleans sibling temporary files");
}

void testExportRejectsManagedApplicationDestinations() {
  for (const int destinationCase : {0, 1, 2}) {
    Fixture fixture;
    const auto source = fixture.manager.pathsFor(fixture.sourceId);
    const auto target = fixture.manager.pathsFor(fixture.targetId);
    const std::filesystem::path destination =
        destinationCase == 0 ? source.settingsJson
        : destinationCase == 1
            ? target.scoresDb
            : fixture.temp.path() / "managed-export.asobprofile";
    const bool existed = std::filesystem::exists(destination);
    const std::string before = existed ? readFile(destination) : std::string{};

    ProfileArchiveService service(fixture.manager);
    const auto result = service.Export(fixture.sourceId, destination);
    expect(result.error == ProfileError::IoFailure,
           "export rejects managed application destination case " +
               std::to_string(destinationCase));
    expect(std::filesystem::exists(destination) == existed &&
               (!existed || readFile(destination) == before) &&
               fixture.manager.validateProfile(fixture.sourceId).ok() &&
               fixture.manager.validateProfile(fixture.targetId).ok(),
           "managed destination rejection preserves all live profile state");
  }
}

void testExportBoundsExistingDestinationBackup() {
  Fixture fixture;
  const auto destination = fixture.exchange.path() / "oversized-existing.zip";
  const auto oversized =
      ProfileArchiveSizePolicy::kMaximumExistingArchiveBytes + 1;
  writeFile(destination, "");
  std::error_code error;
  std::filesystem::resize_file(destination, oversized, error);
  expect(!error && std::filesystem::file_size(destination) == oversized,
         "oversized sparse export destination fixture creates");

  ProfileArchiveService service(fixture.manager);
  const auto result = service.Export(fixture.sourceId, destination);
  expect(result.error == ProfileError::IoFailure &&
             result.message.find("rollback backup limit") != std::string::npos,
         "export rejects an existing destination beyond its backup bound");
  expect(std::filesystem::file_size(destination) == oversized,
         "backup bound rejection preserves the oversized destination");
}

void testExportDetectsExistingDestinationChangesDuringBackup() {
  Fixture fixture;
  const auto destination = fixture.exchange.path() / "changing-existing.zip";
  writeFile(destination, "original-export");
  ProfileArchiveDependencies dependencies;
  dependencies.filesystem.syncFile = [&](const std::filesystem::path &path,
                                         std::string &syncError) {
    if (path.filename().string().starts_with("changing-existing.zip.backup-")) {
      writeFile(destination, "concurrent-change");
    }
    return atomic_file::syncFile(path, syncError);
  };
  ProfileArchiveService service(fixture.manager, std::move(dependencies));
  const auto result = service.Export(fixture.sourceId, destination);
  expect(result.error == ProfileError::IoFailure &&
             result.message.find("changed while preparing") !=
                 std::string::npos,
         "export detects an existing destination changed during backup");
  expect(readFile(destination) == "concurrent-change",
         "concurrent destination change is never replaced by a stale backup");
}

void testExportTemporaryPathsArePrivateAndCleaned() {
  Fixture fixture;
  const auto destination = fixture.exchange.path() / "private.asobprofile";
  bool foundWorkspace = false;
  bool foundArchive = false;
  ProfileArchiveDependencies dependencies;
  dependencies.beforeExportPhase = [&](ProfileArchiveExportPhase,
                                       std::string &phaseError) {
    for (const auto &entry :
         std::filesystem::directory_iterator(fixture.exchange.path())) {
      const std::string name = entry.path().filename().string();
      if (name.starts_with("private.asobprofile.work-")) {
        foundWorkspace = true;
#ifndef _WIN32
        const auto permissions =
            entry.status().permissions() & std::filesystem::perms::mask;
        expect(permissions == std::filesystem::perms::owner_all,
               "export workspace is created atomically with mode 0700");
#else
        expect(hasProtectedCurrentUserOnlyDacl(entry.path()),
               "export workspace has a protected current-user-only DACL");
#endif
      }
      if (name.starts_with("private.asobprofile.tmp-")) {
        foundArchive = true;
#ifndef _WIN32
        const auto permissions =
            entry.status().permissions() & std::filesystem::perms::mask;
        expect(permissions == (std::filesystem::perms::owner_read |
                               std::filesystem::perms::owner_write),
               "export archive temporary is created with mode 0600");
#else
        expect(hasProtectedCurrentUserOnlyDacl(entry.path()),
               "export archive temporary has a protected current-user-only "
               "DACL");
#endif
      }
    }
    phaseError = "stop after inspecting private temporary paths";
    return false;
  };
  ProfileArchiveService service(fixture.manager, std::move(dependencies));
  const auto result = service.Export(fixture.sourceId, destination);
  expect(result.error == ProfileError::IoFailure && foundWorkspace &&
             foundArchive,
         "export exposes only private workspace and archive temporaries");
  std::size_t artifacts = 0;
  for (const auto &entry :
       std::filesystem::directory_iterator(fixture.exchange.path())) {
    if (entry.path().filename().string().starts_with("private.asobprofile.")) {
      ++artifacts;
    }
  }
  expect(artifacts == 0,
         "private export temporary paths are cleaned after failure");
}

void testStaleArchiveWorkspacesAreSweptWithoutTouchingFreshOnes() {
  Fixture fixture;
  const std::string staleToken = generateWorkspaceToken();
  const std::string freshToken = generateWorkspaceToken();
  expect(staleToken != freshToken && isLowerHexToken(staleToken) &&
             isLowerHexToken(freshToken),
         "stale and fresh import workspace tokens are distinct lowercase "
         "32-hex UUID values");
  const auto destination = fixture.exchange.path() / "sweep.asobprofile";
  const auto staleExport =
      fixture.exchange.path() /
      ("sweep.asobprofile.work-" + std::string(staleToken));
  const auto staleTemporary =
      fixture.exchange.path() /
      ("sweep.asobprofile.tmp-" + std::string(staleToken));
  const auto freshExport =
      fixture.exchange.path() /
      ("sweep.asobprofile.work-" + std::string(freshToken));
  writeFile(staleExport / "large-remnant.db", "stale");
  writeFile(staleTemporary, "stale");
  writeFile(freshExport / "active-remnant.db", "fresh");
  const auto staleTime =
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
  std::filesystem::last_write_time(staleExport, staleTime);
  std::filesystem::last_write_time(staleTemporary, staleTime);

  ProfileArchiveService service(fixture.manager);
  const auto exported = service.Export(fixture.sourceId, destination);
  expect(exported.ok() && !std::filesystem::exists(staleExport) &&
             !std::filesystem::exists(staleTemporary) &&
             std::filesystem::exists(freshExport),
         "export startup sweeps stale work/tmp artifacts and retains fresh "
         "ones");

  const auto staleBackup =
      fixture.exchange.path() /
      ("sweep.asobprofile.backup-" + std::string(staleToken));
  const auto freshBackup =
      fixture.exchange.path() /
      ("sweep.asobprofile.backup-" + std::string(freshToken));
  writeFile(staleBackup, "stale-backup");
  writeFile(freshBackup, "fresh-backup");
  std::filesystem::last_write_time(staleBackup, staleTime);
  const auto exportedAgain = service.Export(fixture.sourceId, destination);
  expect(exportedAgain.ok() && !std::filesystem::exists(staleBackup) &&
             std::filesystem::exists(freshBackup),
         "export startup sweeps stale rollback copies only when a current "
         "destination exists and retains fresh copies");

  ScopedDirectoryLock lock(std::filesystem::temp_directory_path() /
                           "asobmashow-profile-import-sweep-test-lock");
  if (!lock.locked()) {
    return;
  }
  const auto staleImport =
      std::filesystem::temp_directory_path() /
      ("asobmashow-profile-import-" + std::string(staleToken));
  const auto freshImport =
      std::filesystem::temp_directory_path() /
      ("asobmashow-profile-import-" + std::string(freshToken));
  writeFile(staleImport / "extracted" / "scores.db", "stale");
  writeFile(freshImport / "extracted" / "scores.db", "fresh");
  std::filesystem::last_write_time(staleImport, staleTime);
  const auto imported = service.Import(destination);
  expect(imported.ok() && !std::filesystem::exists(staleImport) &&
             std::filesystem::exists(freshImport),
         "import startup sweeps stale global workspaces and retains fresh "
         "ones");
  std::error_code ignored;
  std::filesystem::remove_all(freshImport, ignored);
}

void testExportTransactionsPreserveExistingDestination() {
  for (const int failurePoint : {0, 1, 2, 3, 4, 5, 6}) {
    Fixture fixture;
    const auto destination =
        fixture.exchange.path() / "transaction.asobprofile";
    writeFile(destination, "existing-destination");
    int renameCalls = 0;
    int fileSyncCalls = 0;
    int directorySyncCalls = 0;
    ProfileArchiveDependencies dependencies;
    dependencies.filesystem.syncFile = [&](const std::filesystem::path &path,
                                           std::string &syncError) {
      ++fileSyncCalls;
      if (failurePoint == 2 && fileSyncCalls == 2) {
        syncError = "injected export backup sync failure";
        return false;
      }
      return atomic_file::syncFile(path, syncError);
    };
    dependencies.filesystem.removePath = [](const std::filesystem::path &path,
                                            std::string &error) {
      std::error_code removeError;
      std::filesystem::remove_all(path, removeError);
      if (removeError) {
        error = removeError.message();
        return false;
      }
      return true;
    };
    dependencies.filesystem.durableRename =
        [&](const std::filesystem::path &from, const std::filesystem::path &to,
            std::string &renameError) {
          ++renameCalls;
          if (failurePoint == 6 && renameCalls == 1) {
            if (!atomic_file::renameDurably(from, to, renameError)) {
              return false;
            }
            renameError = "injected ambiguous completed rename";
            return false;
          }
          if (failurePoint == 1 && renameCalls == 1) {
            renameError = "injected export rename failure";
            return false;
          }
          return atomic_file::renameDurably(from, to, renameError);
        };
    dependencies.filesystem.syncDirectory =
        [&](const std::filesystem::path &path, std::string &syncError) {
          ++directorySyncCalls;
          if ((failurePoint == 3 && directorySyncCalls == 2) ||
              (failurePoint == 4 && directorySyncCalls == 1)) {
            syncError = "injected export commit sync failure";
            return false;
          }
          return atomic_file::syncDirectory(path, syncError);
        };
    if (failurePoint == 0) {
      dependencies.beforeExportPhase = [](ProfileArchiveExportPhase,
                                          std::string &phaseError) {
        phaseError = "injected post-temporary export failure";
        return false;
      };
    } else if (failurePoint == 5) {
      dependencies.beforeExportPhase = [&](ProfileArchiveExportPhase,
                                           std::string &) {
        for (const auto &entry :
             std::filesystem::directory_iterator(fixture.exchange.path())) {
          if (entry.path().filename().string().starts_with(
                  "transaction.asobprofile.tmp-")) {
            writeFile(entry.path(), "corrupted temporary archive");
            return true;
          }
        }
        return false;
      };
    }

    ProfileArchiveService service(fixture.manager, std::move(dependencies));
    const auto result = service.Export(fixture.sourceId, destination);
    expect(result.error == (failurePoint == 5 ? ProfileError::IntegrityFailure
                                              : ProfileError::IoFailure),
           "injected export transaction failure is reported at point " +
               std::to_string(failurePoint));
    expect(readFile(destination) == "existing-destination",
           "export failure preserves existing destination at point " +
               std::to_string(failurePoint));
    std::size_t artifacts = 0;
    for (const auto &entry :
         std::filesystem::directory_iterator(fixture.exchange.path())) {
      const std::string name = entry.path().filename().string();
      if (name.starts_with("transaction.asobprofile.tmp-") ||
          name.starts_with("transaction.asobprofile.work-") ||
          name.starts_with("transaction.asobprofile.backup-")) {
        ++artifacts;
      }
    }
    expect(artifacts == 0,
           "failed export transaction cleans temporary and backup artifacts");
  }
}

void testStartupRecoversInterruptedProfileOverwrite() {
  {
    Fixture fixture;
    const auto target = fixture.manager.pathsFor(fixture.targetId);
    const auto backup =
        fixture.temp.path() / "profiles" / (".backup-" + fixture.targetId);
    std::filesystem::rename(target.root, backup);
    std::filesystem::create_directory(target.root);
    writeFile(target.profileJson, "{\"partial\":true}\n");

    PlayerProfileManager recovered(fixture.temp.path());
    const auto initialized = recovered.Initialize();
    expect(initialized.ok(),
           "startup restores a valid backup over a partial destination: " +
               initialized.message);
    expect(recovered.validateProfile(fixture.targetId).ok() &&
               scalarText(recovered.pathsFor(fixture.targetId).scoresDb,
                          "SELECT value FROM archive_marker") ==
                   "score-target" &&
               !std::filesystem::exists(backup),
           "partial overwrite recovery retains the complete old profile");
  }

  {
    Fixture fixture;
    const auto target = fixture.manager.pathsFor(fixture.targetId);
    const auto backup =
        fixture.temp.path() / "profiles" / (".backup-" + fixture.targetId);
    std::filesystem::rename(target.root, backup);
    std::filesystem::create_directory(target.root);
    writeFile(target.profileJson, "{\"partial\":true}\n");

    PlayerProfileManagerDependencies dependencies;
    dependencies.filesystem.durableRename =
        [&](const std::filesystem::path &from, const std::filesystem::path &to,
            std::string &error) {
          if (from == backup && to == target.root) {
            error = "injected backup restoration failure";
            return false;
          }
          return atomic_file::renameDurably(from, to, error);
        };
    PlayerProfileManager recovered(fixture.temp.path(),
                                   std::move(dependencies));
    const auto initialized = recovered.Initialize();
    expect(!initialized.ok(), "failed startup restoration is reported");
    expect(std::filesystem::exists(backup) &&
               scalarText(backup / "scores.db",
                          "SELECT value FROM archive_marker") == "score-target",
           "failed restoration retains the only complete backup");
  }

  {
    Fixture fixture;
    const auto target = fixture.manager.pathsFor(fixture.targetId);
    const auto backup =
        fixture.temp.path() / "profiles" / (".backup-" + fixture.targetId);
    std::filesystem::rename(target.root, backup);
    std::filesystem::remove(backup / "scores.db");
    std::filesystem::create_directory(target.root);
    writeFile(target.profileJson, "{\"partial\":true}\n");

    PlayerProfileManager recovered(fixture.temp.path());
    const auto initialized = recovered.Initialize();
    expect(!initialized.ok(),
           "startup fails closed when destination and backup are invalid");
    expect(std::filesystem::exists(target.root) &&
               std::filesystem::exists(backup),
           "ambiguous invalid recovery state retains both artifacts");
  }

  {
    Fixture fixture;
    const auto target = fixture.manager.pathsFor(fixture.targetId);
    const auto backup =
        fixture.temp.path() / "profiles" / (".backup-" + fixture.targetId);
    std::filesystem::copy(target.root, backup,
                          std::filesystem::copy_options::recursive);
    Json futureSettings = Json::parse(readFile(target.settingsJson));
    futureSettings["schemaVersion"] =
        AppSettingsStore::kCurrentSchemaVersion + 1;
    writeFile(target.settingsJson, futureSettings.dump(2) + "\n");
    const std::string futureBytes = readFile(target.settingsJson);

    PlayerProfileManager recovered(fixture.temp.path());
    const auto initialized = recovered.Initialize();
    expect(!initialized.ok(),
           "startup fails closed for a newer destination beside a backup");
    expect(std::filesystem::exists(target.root) &&
               std::filesystem::exists(backup) &&
               readFile(target.settingsJson) == futureBytes,
           "future-version recovery retains both complete versions");
  }
}

void testOverwriteFaultMatrixRecoversOneCompleteProfile() {
  for (int failurePoint = 0; failurePoint <= 10; ++failurePoint) {
    TempDirectory temp{"profile-overwrite-fault"};
    TempDirectory exchange{"profile-overwrite-fault-exchange"};
    const std::string activeId = "11111111-1111-4111-8111-111111111111";
    const std::string sourceId = "22222222-2222-4222-8222-222222222222";
    const std::string targetId = "33333333-3333-4333-8333-333333333333";
    std::vector<std::string> uuids{activeId, sourceId, targetId};
    std::size_t uuidIndex = 0;
    bool inject = false;
    bool commitSyncFailed = false;
    const auto profiles = temp.path() / "profiles";
    const auto target = profiles / targetId;
    const auto backup = profiles / (".backup-" + targetId);

    PlayerProfileManagerDependencies dependencies;
    dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
    dependencies.utcNow = [] { return std::string("2026-07-11T01:23:45Z"); };
    dependencies.filesystem.durableRename =
        [&](const std::filesystem::path &from, const std::filesystem::path &to,
            std::string &renameError) {
          if (!inject) {
            return atomic_file::renameDurably(from, to, renameError);
          }
          if (from == target && to == backup) {
            if (failurePoint == 0) {
              renameError = "injected pre-backup rename failure";
              return false;
            }
            if (failurePoint == 1) {
              if (!atomic_file::renameDurably(from, to, renameError)) {
                return false;
              }
              renameError = "injected ambiguous backup rename";
              return false;
            }
          }
          if (from.filename().string().starts_with(".staging-") &&
              to == target) {
            if (failurePoint == 3) {
              renameError = "injected pre-replacement rename failure";
              return false;
            }
            if (failurePoint == 4) {
              if (!atomic_file::renameDurably(from, to, renameError)) {
                return false;
              }
              renameError = "injected ambiguous replacement rename";
              return false;
            }
          }
          if (from == backup && to == target && failurePoint == 7) {
            renameError = "injected rollback restoration failure";
            return false;
          }
          return atomic_file::renameDurably(from, to, renameError);
        };
    dependencies.filesystem.removeTree = [&](const std::filesystem::path &path,
                                             std::string &removeError) {
      if (inject && failurePoint == 6 && path == target) {
        removeError = "injected rollback removal failure";
        return false;
      }
      std::error_code error;
      std::filesystem::remove_all(path, error);
      if (error) {
        removeError = error.message();
        return false;
      }
      return true;
    };
    dependencies.filesystem.syncDirectory =
        [&](const std::filesystem::path &path, std::string &syncError) {
          if (!inject || path != profiles) {
            return atomic_file::syncDirectory(path, syncError);
          }
          const bool backupExists = std::filesystem::exists(backup);
          const bool targetExists = std::filesystem::exists(target);
          if (failurePoint == 2 && backupExists && !targetExists) {
            syncError = "injected overwrite backup sync failure";
            return false;
          }
          if (failurePoint >= 5 && !commitSyncFailed && backupExists &&
              targetExists) {
            if (failurePoint == 9) {
              std::filesystem::remove_all(backup);
            } else if (failurePoint == 10) {
              std::filesystem::remove(backup / "scores.db");
            }
            commitSyncFailed = true;
            syncError = "injected overwrite commit sync failure";
            return false;
          }
          if (failurePoint == 8 && commitSyncFailed && !backupExists &&
              targetExists) {
            syncError = "injected overwrite rollback sync failure";
            return false;
          }
          return atomic_file::syncDirectory(path, syncError);
        };

    const auto archive = exchange.path() / "fault-source.zip";
    expect(copyFaultArchiveFixture(overwriteFaultFixtureSeed(), temp.path(),
                                   archive),
           "overwrite fault fixture seed copies at point " +
               std::to_string(failurePoint));
    uuidIndex = 3;
    PlayerProfileManager manager(temp.path(), std::move(dependencies));
    expect(manager.Initialize().ok(),
           "overwrite fault fixture initializes at point " +
               std::to_string(failurePoint));
    ProfileArchiveService service(manager);

    inject = true;
    const ProfileImportOptions options{.mode = ProfileImportMode::Overwrite,
                                       .overwriteProfileId = targetId};
    const auto imported = service.Import(archive, options);
    expect(imported.error == ProfileError::IoFailure,
           "overwrite boundary fault is reported at point " +
               std::to_string(failurePoint));
    inject = false;

    PlayerProfileManager recovered(temp.path());
    const auto initialized = recovered.Initialize();
    expect(initialized.ok() && recovered.validateProfile(targetId).ok(),
           "restart resolves overwrite boundary fault at point " +
               std::to_string(failurePoint) + ": " + initialized.message);
    const std::string expected =
        failurePoint == 6 || failurePoint == 9 || failurePoint == 10
            ? "new-data"
            : "old-data";
    expect(scalarText(recovered.pathsFor(targetId).scoresDb,
                      "SELECT value FROM archive_marker") == expected &&
               transactionArtifacts(temp.path()).empty(),
           "overwrite fault leaves exactly one complete recoverable profile at "
           "point " +
               std::to_string(failurePoint));
  }
}

void testCreateImportFaultMatrixHasUnambiguousOutcome() {
  for (int failurePoint = 0; failurePoint <= 7; ++failurePoint) {
    TempDirectory temp{"profile-create-fault"};
    TempDirectory exchange{"profile-create-fault-exchange"};
    const std::string activeId = "11111111-1111-4111-8111-111111111111";
    const std::string sourceId = "22222222-2222-4222-8222-222222222222";
    const std::string importedId = "33333333-3333-4333-8333-333333333333";
    std::vector<std::string> uuids{activeId, sourceId, importedId};
    std::size_t uuidIndex = 0;
    bool inject = false;
    bool commitSyncFailed = false;
    const auto profiles = temp.path() / "profiles";
    const auto destination = profiles / importedId;
    const auto staging = profiles / (".staging-" + importedId);

    PlayerProfileManagerDependencies dependencies;
    dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
    dependencies.utcNow = [] { return std::string("2026-07-11T01:23:45Z"); };
    dependencies.filesystem.durableRename =
        [&](const std::filesystem::path &from, const std::filesystem::path &to,
            std::string &renameError) {
          if (!inject) {
            return atomic_file::renameDurably(from, to, renameError);
          }
          if (from == staging && to == destination) {
            if (failurePoint == 0) {
              renameError = "injected create finalization failure";
              return false;
            }
            if (failurePoint == 1) {
              if (!atomic_file::renameDurably(from, to, renameError)) {
                return false;
              }
              renameError = "injected ambiguous create finalization";
              return false;
            }
          }
          if (from == destination && to == staging) {
            if (failurePoint == 3 || failurePoint == 7) {
              renameError = "injected create rollback rename failure";
              return false;
            }
            if (failurePoint == 4) {
              if (!atomic_file::renameDurably(from, to, renameError)) {
                return false;
              }
              renameError = "injected ambiguous create rollback rename";
              return false;
            }
          }
          return atomic_file::renameDurably(from, to, renameError);
        };
    dependencies.filesystem.syncDirectory =
        [&](const std::filesystem::path &path, std::string &syncError) {
          if (inject && path == profiles && failurePoint >= 2 &&
              !commitSyncFailed) {
            commitSyncFailed = true;
            syncError = "injected create commit sync failure";
            return false;
          }
          if (inject && path == profiles &&
              (failurePoint == 6 || failurePoint == 7) && commitSyncFailed) {
            syncError = "injected create rollback sync failure";
            return false;
          }
          return atomic_file::syncDirectory(path, syncError);
        };
    dependencies.filesystem.removeTree = [&](const std::filesystem::path &path,
                                             std::string &removeError) {
      if (inject && failurePoint == 5 && path == staging) {
        removeError = "injected create rollback cleanup failure";
        return false;
      }
      std::error_code error;
      std::filesystem::remove_all(path, error);
      if (error) {
        removeError = error.message();
        return false;
      }
      return true;
    };

    const auto archive = exchange.path() / "create-fault-source.zip";
    expect(copyFaultArchiveFixture(createFaultFixtureSeed(), temp.path(),
                                   archive),
           "create fault fixture seed copies at point " +
               std::to_string(failurePoint));
    uuidIndex = 2;
    PlayerProfileManager manager(temp.path(), std::move(dependencies));
    expect(manager.Initialize().ok(),
           "create fault fixture initializes at point " +
               std::to_string(failurePoint));
    ProfileArchiveService service(manager);

    inject = true;
    const auto imported = service.Import(archive);
    const bool expectedCommitted =
        failurePoint == 1 || failurePoint == 3 || failurePoint == 7;
    expect(imported.ok() == expectedCommitted,
           "create import reports its durable visible outcome at point " +
               std::to_string(failurePoint));
    if (failurePoint == 7) {
      expect(!imported.message.empty(),
             "retained create commit reports its repeated sync warning");
    }
    inject = false;

    PlayerProfileManager recovered(temp.path());
    const auto initialized = recovered.Initialize();
    expect(initialized.ok(), "create import fault recovers at point " +
                                 std::to_string(failurePoint) + ": " +
                                 initialized.message);
    const auto validation = recovered.validateProfile(importedId);
    expect(
        validation.ok() == expectedCommitted &&
            transactionArtifacts(temp.path()).empty(),
        "create fault leaves exactly the reported profile outcome at point " +
            std::to_string(failurePoint));
    if (expectedCommitted) {
      expect(scalarText(recovered.pathsFor(importedId).scoresDb,
                        "SELECT value FROM archive_marker") == "imported-data",
             "committed create fault retains imported payload");
    }
  }
}

void testCommittedOverwriteSurvivesBackupCleanupFailures() {
  for (const int cleanupFailure : {0, 1, 2}) {
    TempDirectory temp{"profile-overwrite-cleanup"};
    TempDirectory exchange{"profile-overwrite-cleanup-exchange"};
    const std::string activeId = "11111111-1111-4111-8111-111111111111";
    const std::string sourceId = "22222222-2222-4222-8222-222222222222";
    const std::string targetId = "33333333-3333-4333-8333-333333333333";
    const std::string importId = "44444444-4444-4444-8444-444444444444";
    std::vector<std::string> uuids{activeId, sourceId, targetId, importId};
    std::size_t uuidIndex = 0;
    bool injectCleanup = false;
    int profileSyncs = 0;
    PlayerProfileManagerDependencies dependencies;
    dependencies.generateUuid = [&] { return uuids.at(uuidIndex++); };
    dependencies.utcNow = [] { return std::string("2026-07-11T01:23:45Z"); };
    dependencies.filesystem.removeTree = [&](const std::filesystem::path &path,
                                             std::string &removeError) {
      if (injectCleanup && path.filename().string().starts_with(".backup-") &&
          cleanupFailure != 2) {
        if (cleanupFailure == 1) {
          std::error_code ignored;
          std::filesystem::remove(path / "profile.json", ignored);
        }
        removeError = "injected backup cleanup failure";
        return false;
      }
      std::error_code error;
      std::filesystem::remove_all(path, error);
      if (error) {
        removeError = error.message();
        return false;
      }
      return true;
    };
    dependencies.filesystem.syncDirectory =
        [&](const std::filesystem::path &path, std::string &syncError) {
          if (injectCleanup && path == temp.path() / "profiles") {
            ++profileSyncs;
            if (cleanupFailure == 2 && profileSyncs == 3) {
              syncError = "injected post-commit cleanup sync failure";
              return false;
            }
          }
          return atomic_file::syncDirectory(path, syncError);
        };
    const auto archive = exchange.path() / "cleanup.zip";
    expect(copyFaultArchiveFixture(cleanupFaultFixtureSeed(), temp.path(),
                                   archive),
           "cleanup fixture seed copies at point " +
               std::to_string(cleanupFailure));
    uuidIndex = 3;
    PlayerProfileManager manager(temp.path(), std::move(dependencies));
    expect(manager.Initialize().ok(), "cleanup fixture initializes");
    ProfileArchiveService service(manager);

    injectCleanup = true;
    ProfileImportOptions options{.mode = ProfileImportMode::Overwrite,
                                 .overwriteProfileId = targetId};
    const auto imported = service.Import(archive, options);
    expect(imported.ok() && !imported.message.empty(),
           "post-commit cleanup failure reports a success warning, not "
           "rollback");
    expect(manager.validateProfile(targetId).ok() &&
               scalarText(manager.pathsFor(targetId).scoresDb,
                          "SELECT value FROM archive_marker") == "new-data",
           "committed overwrite remains installed after cleanup failure");

    injectCleanup = false;
    PlayerProfileManager recovered(temp.path());
    expect(recovered.Initialize().ok(),
           "next startup cleans an abandoned committed backup");
    expect(recovered.validateProfile(targetId).ok() &&
               transactionArtifacts(temp.path()).empty(),
           "startup keeps committed overwrite and removes backup artifact");
  }
}
void runPortableShard() {
  testTempDirectoryRetriesOccupiedCandidate();
  testFixtureSeedIsBuiltOnceAndClonesAreIsolated();
  testStreamingSha256();
  testExportIsDeterministicAndStrict();
  testReplayFilesRoundTripByVerifiedOwnership();
  testIrOperationalStateIsNotProfilePortable();
  testExportRejectsSupportedOlderSourceBeforeWritingArchive();
  testPresetStoreSidecarRemainsProfilePortable();
  testMalformedOptionalPracticeRemainsVisibleButCannotExport();
  testArchiveUsesNativeUnicodeFilesystemPaths();
  testCreateImportUsesNewIdAndRoundTripsExactly();
  testVersionOneArchiveImportsWithEmptyPracticeDirectory();
  testCreateImportRetriesUnsafeAndOccupiedGeneratedIds();
  testOverwriteIsRestrictedAndReplacesInactiveProfile();
  testOverwriteAcceptsSupportedOlderTargetAndInstallsCurrentProfile();
  testOverwriteRejectsFutureTargetWithoutMutation();
  testOverwriteRefusesTheLastProfile();
  testOverwriteRollbackRestoresOriginalProfile();
}

void runValidationShard() {
  testStrictMemberAllowlistAndTypes();
  testPracticeMembersAreValidatedBeforeInstall();
  testChecksumsVersionsValidatorsAndLimits();
  testSizePolicyBoundariesWithoutLargeAllocations();
  testZipParserEnforcesDeclaredAndStreamedSizeLimits();
  testSupportedOlderSchemasMigrateAndPreserveRows();
  testFutureDatabaseAndCorruptionAreRejected();
}

void runTransactionsShard() {
  testExportFailurePreservesDestinationAndCleansTemps();
  testExportRejectsManagedApplicationDestinations();
  testExportBoundsExistingDestinationBackup();
  testExportDetectsExistingDestinationChangesDuringBackup();
  testExportTemporaryPathsArePrivateAndCleaned();
  testStaleArchiveWorkspacesAreSweptWithoutTouchingFreshOnes();
  testExportTransactionsPreserveExistingDestination();
  testStartupRecoversInterruptedProfileOverwrite();
}

void runFaultsShard() {
  testOverwriteFaultMatrixRecoversOneCompleteProfile();
  testCreateImportFaultMatrixHasUnambiguousOutcome();
  testCommittedOverwriteSurvivesBackupCleanupFailures();
}

bool runShard(std::string_view shard) {
  if (shard == "portable") {
    runPortableShard();
    return true;
  }
  if (shard == "validation") {
    runValidationShard();
    return true;
  }
  if (shard == "transactions") {
    runTransactionsShard();
    return true;
  }
  if (shard == "faults") {
    runFaultsShard();
    return true;
  }
  return false;
}

void printShardUsage(const char *program) {
  std::cerr << "Usage: " << program
            << " [--shard portable|validation|transactions|faults]\n";
}
} // namespace

int main(int argc, char *argv[]) {
  if (argc == 1) {
    runPortableShard();
    runValidationShard();
    runTransactionsShard();
    runFaultsShard();
  } else if (argc == 3 && std::string_view(argv[1]) == "--shard" &&
             runShard(argv[2])) {
  } else {
    std::cerr << "Unsupported shard selector.\n";
    printShardUsage(argv[0]);
    return 2;
  }

  if (failures != 0) {
    std::cerr << failures << " profile archive test(s) failed\n";
    return 1;
  }
  std::cout << "All profile archive tests passed\n";
  return 0;
}
