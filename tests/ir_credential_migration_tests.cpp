#include "ir/IrCredentialMigration.h"

#include "ir/IrCredentialStore.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-ir-migration-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

class FakeBackend final : public ir::IrCredentialBackend {
public:
  bool requiresLegacyFileMigration() const noexcept override { return true; }

  ir::IrCredentialBackendReadResult
  load(std::string_view profileId, std::string_view providerId) noexcept override {
    events.push_back("load:" + std::string(profileId) + ":" +
                     std::string(providerId));
    if (failVerificationProvider == providerId) {
      return {.status = ir::IrCredentialBackendReadStatus::Loaded,
              .apiKey = "different-key"};
    }
    const auto found = keys.find({std::string(profileId),
                                  std::string(providerId)});
    if (found == keys.end()) {
      return {.status = ir::IrCredentialBackendReadStatus::Missing};
    }
    return {.status = ir::IrCredentialBackendReadStatus::Loaded,
            .apiKey = found->second};
  }

  ir::IrCredentialBackendWriteResult
  replace(std::string_view profileId, std::string_view providerId,
          std::string_view apiKey) noexcept override {
    events.push_back("replace:" + std::string(profileId) + ":" +
                     std::string(providerId));
    ++replaceCalls;
    if (replaceCalls == failReplaceCall) {
      return {.succeeded = false,
              .diagnostic = "backend echoed " + std::string(apiKey)};
    }
    keys[{std::string(profileId), std::string(providerId)}] = apiKey;
    return {.succeeded = true};
  }

  ir::IrCredentialBackendWriteResult
  remove(std::string_view profileId,
         std::string_view providerId) noexcept override {
    keys.erase({std::string(profileId), std::string(providerId)});
    return {.succeeded = true};
  }

  ir::IrCredentialBackendWriteResult
  removeProfile(std::string_view profileId) noexcept override {
    for (auto iterator = keys.begin(); iterator != keys.end();) {
      if (iterator->first.first == profileId) {
        iterator = keys.erase(iterator);
      } else {
        ++iterator;
      }
    }
    return {.succeeded = true};
  }

  std::map<std::pair<std::string, std::string>, std::string> keys;
  std::vector<std::string> events;
  int replaceCalls = 0;
  int failReplaceCall = -1;
  std::string failVerificationProvider;
};

constexpr std::string_view kProfileId =
    "11111111-1111-4111-8111-111111111111";

std::filesystem::path seedLegacyFile(const TempDirectory &temp) {
  const auto path = temp.path() / "ir-credentials.json";
  ir::IrCredentials credentials;
  credentials.apiKeys = {{"alpha", "first-secret"},
                         {"tachi", "second-secret"}};
  expect(ir::IrCredentialStore::save(path, credentials).succeeded,
         "legacy fixture saves");
  return path;
}

void writeFile(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void testMissingLegacyFileNeedsNoBackendMutation() {
  TempDirectory temp;
  FakeBackend backend;
  const auto missing = temp.path() / "missing.json";
  writeFile(missing.string() + ".bak", "orphaned-secret");
  const auto result = ir::migrateLegacyIrCredentials(
      kProfileId, missing, backend);
  expect(result.status == ir::IrCredentialMigrationStatus::NotNeeded &&
             result.ready(),
         "missing legacy file is ready without migration");
  expect(backend.events.empty(), "missing migration does not touch backend");
  expect(std::filesystem::is_empty(temp.path()),
         "missing migration removes orphaned credential backups");
}

void testFileBackendPreservesProfileIsolationAndRemoval() {
  TempDirectory temp;
  auto backend = ir::CreatePlatformIrCredentialBackend(temp.path());
  expect(backend && !backend->requiresLegacyFileMigration(),
         "desktop backend remains file based");
  constexpr std::string_view secondProfile =
      "22222222-2222-4222-8222-222222222222";
  expect(backend->replace(kProfileId, "tachi", "first-profile-key").succeeded &&
             backend->replace(secondProfile, "tachi", "second-profile-key")
                 .succeeded,
         "file backend stores credentials for separate profiles");
  expect(backend->removeProfile(kProfileId).succeeded,
         "profile-wide credential removal succeeds");
  expect(backend->load(kProfileId, "tachi").status ==
                 ir::IrCredentialBackendReadStatus::Missing &&
             backend->load(secondProfile, "tachi").apiKey ==
                 "second-profile-key",
         "profile removal cannot delete another profile's credential");
}

void testMigratingFileBackendMovesLegacyCredentialsIntoPrivateRoot() {
  TempDirectory temp;
  const auto externalRoot = temp.path() / "external";
  const auto privateRoot = temp.path() / "private";
  const auto legacyPath = externalRoot / "profiles" /
                          std::string(kProfileId) / "ir-credentials.json";
  std::filesystem::create_directories(legacyPath.parent_path());

  ir::IrCredentials credentials;
  credentials.apiKeys = {{"tachi", "private-storage-secret"}};
  expect(ir::IrCredentialStore::save(legacyPath, credentials).succeeded,
         "Android legacy credential fixture saves in external storage");

  auto backend = ir::detail::CreateFileIrCredentialBackend(
      privateRoot, true);
  expect(backend && backend->requiresLegacyFileMigration(),
         "Android private file backend requests legacy migration");

  const auto result = ir::migrateLegacyIrCredentials(
      kProfileId, legacyPath, *backend);
  const auto loaded = backend->load(kProfileId, "tachi");
  expect(result.status == ir::IrCredentialMigrationStatus::Succeeded &&
             loaded.status == ir::IrCredentialBackendReadStatus::Loaded &&
             loaded.apiKey == "private-storage-secret",
         "legacy credential migrates into the private backend");
  expect(!std::filesystem::exists(legacyPath),
         "verified migration removes the external plaintext credential");
  expect(std::filesystem::exists(privateRoot / "profiles" /
                                 std::string(kProfileId) /
                                 "ir-credentials.json"),
         "migrated credential is stored under the private root");
}

void testMigrationWritesVerifiesThenDeletes() {
  TempDirectory temp;
  const auto path = seedLegacyFile(temp);
  FakeBackend backend;
  std::vector<std::string> cleanupEvents;
  ir::IrCredentialMigrationOperations operations{
      .removeBackupArtifacts =
          [&](const auto &, std::string &) {
            cleanupEvents.emplace_back("artifacts");
            return true;
          },
      .removeLegacyFile =
          [&](const auto &candidate, std::string &) {
            cleanupEvents.emplace_back("file");
            std::error_code error;
            return std::filesystem::remove(candidate, error) && !error;
          },
      .syncDirectory =
          [&](const auto &, std::string &) {
            cleanupEvents.emplace_back("directory");
            return true;
          }};

  const auto result = ir::migrateLegacyIrCredentials(
      kProfileId, path, backend, &operations);
  expect(result.status == ir::IrCredentialMigrationStatus::Succeeded &&
             result.migratedCredentials == 2 && result.ready(),
         "complete migration succeeds");
  expect(backend.events ==
             std::vector<std::string>{
                 "replace:11111111-1111-4111-8111-111111111111:alpha",
                 "load:11111111-1111-4111-8111-111111111111:alpha",
                 "replace:11111111-1111-4111-8111-111111111111:tachi",
                 "load:11111111-1111-4111-8111-111111111111:tachi"},
         "each credential is verified immediately after its write");
  expect(cleanupEvents ==
             std::vector<std::string>{"artifacts", "file", "directory"},
         "secret-bearing backups are cleaned before durable file removal");
  expect(!std::filesystem::exists(path),
         "verified migration deletes the plaintext file");
}

void testPartialWriteAndVerificationFailuresKeepLegacyFile() {
  TempDirectory temp;
  const auto path = seedLegacyFile(temp);
  FakeBackend backend;
  backend.failReplaceCall = 2;

  auto result =
      ir::migrateLegacyIrCredentials(kProfileId, path, backend);
  expect(result.status == ir::IrCredentialMigrationStatus::Failed &&
             !result.ready() && std::filesystem::exists(path),
         "partial backend failure keeps legacy source for retry");
  expect(result.diagnostic.find("second-secret") == std::string::npos,
         "migration failure does not expose a rejected key");

  backend.failReplaceCall = -1;
  backend.failVerificationProvider = "tachi";
  result = ir::migrateLegacyIrCredentials(kProfileId, path, backend);
  expect(result.status == ir::IrCredentialMigrationStatus::Failed &&
             std::filesystem::exists(path),
         "verification mismatch keeps legacy source");

  backend.failVerificationProvider.clear();
  result = ir::migrateLegacyIrCredentials(kProfileId, path, backend);
  expect(result.status == ir::IrCredentialMigrationStatus::Succeeded &&
             !std::filesystem::exists(path),
         "a later launch can retry a partial migration safely");
}

void testInvalidLegacyAndCleanupFailuresFailClosed() {
  TempDirectory malformedTemp;
  const auto malformed = malformedTemp.path() / "ir-credentials.json";
  writeFile(malformed, R"({"schemaVersion":1,"providers":{"tachi":{"apiKey":7}}})");
  FakeBackend malformedBackend;
  auto result = ir::migrateLegacyIrCredentials(kProfileId, malformed,
                                                malformedBackend);
  expect(result.status == ir::IrCredentialMigrationStatus::Failed &&
             malformedBackend.events.empty() &&
             std::filesystem::exists(malformed),
         "malformed legacy credentials fail without partial exposure");

  TempDirectory cleanupTemp;
  const auto cleanupPath = seedLegacyFile(cleanupTemp);
  FakeBackend cleanupBackend;
  bool fileRemovalCalled = false;
  ir::IrCredentialMigrationOperations operations{
      .removeBackupArtifacts =
          [](const auto &, std::string &diagnostic) {
            diagnostic = "injected cleanup failure";
            return false;
          },
      .removeLegacyFile =
          [&](const auto &, std::string &) {
            fileRemovalCalled = true;
            return true;
          },
      .syncDirectory =
          [](const auto &, std::string &) {
            return true;
          }};
  result = ir::migrateLegacyIrCredentials(kProfileId, cleanupPath,
                                           cleanupBackend, &operations);
  expect(result.status == ir::IrCredentialMigrationStatus::Failed &&
             std::filesystem::exists(cleanupPath) && !fileRemovalCalled,
         "cleanup failure preserves the canonical recovery source");
}

void testDirectorySyncFailureKeepsMigrationFailed() {
  TempDirectory temp;
  const auto path = seedLegacyFile(temp);
  FakeBackend backend;
  bool directorySyncCalled = false;
  ir::IrCredentialMigrationOperations operations{
      .removeBackupArtifacts =
          [](const auto &, std::string &) {
            return true;
          },
      .removeLegacyFile =
          [](const auto &candidate, std::string &) {
            std::error_code error;
            return std::filesystem::remove(candidate, error) && !error;
          },
      .syncDirectory =
          [&](const auto &, std::string &diagnostic) {
            directorySyncCalled = true;
            diagnostic = "injected directory sync failure";
            return false;
          }};

  const auto result = ir::migrateLegacyIrCredentials(
      kProfileId, path, backend, &operations);
  expect(result.status == ir::IrCredentialMigrationStatus::Failed &&
             !result.ready() && directorySyncCalled &&
             !std::filesystem::exists(path),
         "migration does not report success after an undurable unlink");
}

} // namespace

int main() {
  testMissingLegacyFileNeedsNoBackendMutation();
  testFileBackendPreservesProfileIsolationAndRemoval();
  testMigratingFileBackendMovesLegacyCredentialsIntoPrivateRoot();
  testMigrationWritesVerifiesThenDeletes();
  testPartialWriteAndVerificationFailuresKeepLegacyFile();
  testInvalidLegacyAndCleanupFailuresFailClosed();
  testDirectorySyncFailureKeepsMigrationFailed();
  if (failures != 0) {
    std::cerr << failures << " IR credential migration test(s) failed\n";
    return 1;
  }
  std::cout << "IR credential migration tests passed\n";
  return 0;
}
