#include "PlayerProfileManager.h"

#include "AppDatabaseInitializer.h"
#include "AppSettingsStore.h"
#include "ProfileDatabaseTools.h"
#include "ReplayDBHelper.h"
#include "ScoreDBHelper.h"
#include "VersionedJson.h"
#include "input/InputProfileStore.h"

#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {
using Json = nlohmann::json;

enum class BuildMode { Migration, Create, Duplicate };

struct BuildProfileResult {
  ProfileResult result;
  PlayerProfilePaths paths;
  bool finalized = false;
};

ProfileResult failure(ProfileError error, std::string message) {
  return {
      .error = error, .message = std::move(message), .profile = std::nullopt};
}

ProfileResult success(PlayerProfile profile) {
  return {.error = ProfileError::None,
          .message = {},
          .profile = std::move(profile)};
}

std::string defaultUuid() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random;
  for (unsigned char &value : bytes) {
    value = static_cast<unsigned char>(random());
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

  std::ostringstream encoded;
  encoded << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      encoded << '-';
    }
    encoded << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return encoded.str();
}

std::string defaultUtcNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t raw = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &raw);
#else
  gmtime_r(&raw, &utc);
#endif
  std::ostringstream encoded;
  encoded << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return encoded.str();
}

bool isUuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') {
        return false;
      }
      continue;
    }
    if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> normalizedName(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  const std::string result(value.substr(begin, end - begin));
  if (result.empty() || result.size() > 256) {
    return std::nullopt;
  }

  std::size_t index = 0;
  std::size_t codePoints = 0;
  while (index < result.size()) {
    const unsigned char first = static_cast<unsigned char>(result[index]);
    if (first < 0x20U || first == 0x7fU) {
      return std::nullopt;
    }
    std::uint32_t codePoint = 0;
    std::size_t length = 0;
    if (first < 0x80U) {
      codePoint = first;
      length = 1;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      codePoint = first & 0x1fU;
      length = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      codePoint = first & 0x0fU;
      length = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      codePoint = first & 0x07U;
      length = 4;
    } else {
      return std::nullopt;
    }
    if (index + length > result.size()) {
      return std::nullopt;
    }
    for (std::size_t continuation = 1; continuation < length; ++continuation) {
      const unsigned char byte =
          static_cast<unsigned char>(result[index + continuation]);
      if ((byte & 0xc0U) != 0x80U) {
        return std::nullopt;
      }
      codePoint = (codePoint << 6U) | (byte & 0x3fU);
    }
    if ((length == 2 && codePoint < 0x80U) ||
        (length == 3 && codePoint < 0x800U) ||
        (length == 4 && codePoint < 0x10000U) ||
        (codePoint >= 0x7fU && codePoint <= 0x9fU) ||
        (codePoint >= 0xd800U && codePoint <= 0xdfffU) ||
        codePoint > 0x10ffffU) {
      return std::nullopt;
    }
    ++codePoints;
    if (codePoints > 80) {
      return std::nullopt;
    }
    index += length;
  }
  return result;
}

PlayerProfilePaths makePaths(const std::filesystem::path &applicationRoot,
                             std::string_view id) {
  if (!isUuid(id)) {
    return {};
  }
  PlayerProfilePaths paths;
  paths.root = applicationRoot / "profiles" / std::string(id);
  paths.profileJson = paths.root / "profile.json";
  paths.settingsJson = paths.root / "settings.json";
  paths.inputJson = paths.root / "input.json";
  paths.scoresDb = paths.root / "scores.db";
  paths.replaysDb = paths.root / "replays.db";
  return paths;
}

PlayerProfilePaths
makeStagingPaths(const std::filesystem::path &applicationRoot,
                 std::string_view id) {
  PlayerProfilePaths paths;
  paths.root = applicationRoot / "profiles" / (".staging-" + std::string(id));
  paths.profileJson = paths.root / "profile.json";
  paths.settingsJson = paths.root / "settings.json";
  paths.inputJson = paths.root / "input.json";
  paths.scoresDb = paths.root / "scores.db";
  paths.replaysDb = paths.root / "replays.db";
  return paths;
}

Json profileJson(const PlayerProfile &profile) {
  return {{"schemaVersion", profile.schemaVersion},
          {"id", profile.id},
          {"displayName", profile.displayName},
          {"createdAt", profile.createdAt},
          {"lastUsedAt", profile.lastUsedAt}};
}

Json bootstrapJson(std::string_view id) {
  return {{"schemaVersion", kActiveProfileSchemaVersion},
          {"activeProfileId", id}};
}

const std::array<versioned_json::Migration, 1> kNoopV0Migration = {
    [](Json &, std::string &) { return true; }};

ProfileResult loadProfileMetadata(const std::filesystem::path &path,
                                  std::string_view expectedId = {}) {
  const auto loaded = versioned_json::loadAndMigrate(
      path, kPlayerProfileSchemaVersion, kNoopV0Migration);
  if (loaded.status == versioned_json::LoadStatus::FutureVersion) {
    return failure(ProfileError::FutureVersion,
                   "profile schema is newer than this application");
  }
  if (loaded.status == versioned_json::LoadStatus::Missing) {
    return failure(ProfileError::NotFound, "profile metadata is missing");
  }
  if (loaded.status != versioned_json::LoadStatus::Loaded) {
    return failure(ProfileError::IntegrityFailure,
                   loaded.diagnostics.empty() ? "profile metadata is invalid"
                                              : loaded.diagnostics.front());
  }
  try {
    PlayerProfile profile;
    profile.schemaVersion = loaded.document.at("schemaVersion").get<int>();
    profile.id = loaded.document.at("id").get<std::string>();
    profile.displayName = loaded.document.at("displayName").get<std::string>();
    profile.createdAt = loaded.document.at("createdAt").get<std::string>();
    profile.lastUsedAt = loaded.document.at("lastUsedAt").get<std::string>();
    const auto validName = normalizedName(profile.displayName);
    if (!isUuid(profile.id) || !validName.has_value() ||
        *validName != profile.displayName || profile.createdAt.empty() ||
        profile.lastUsedAt.empty() ||
        (!expectedId.empty() && profile.id != expectedId)) {
      return failure(ProfileError::IntegrityFailure,
                     "profile metadata does not match its directory");
    }
    return success(std::move(profile));
  } catch (const Json::exception &error) {
    return failure(ProfileError::IntegrityFailure,
                   std::string("profile metadata is invalid: ") + error.what());
  }
}

enum class BootstrapStatus { Missing, Loaded, Invalid, FutureVersion };
struct BootstrapResult {
  BootstrapStatus status = BootstrapStatus::Missing;
  std::string id;
  std::string message;
};

BootstrapResult loadBootstrap(const std::filesystem::path &path) {
  const auto loaded = versioned_json::loadAndMigrate(
      path, kActiveProfileSchemaVersion, kNoopV0Migration);
  if (loaded.status == versioned_json::LoadStatus::Missing) {
    return {};
  }
  if (loaded.status == versioned_json::LoadStatus::FutureVersion) {
    return {.status = BootstrapStatus::FutureVersion,
            .message = "active profile bootstrap is newer than supported"};
  }
  if (loaded.status != versioned_json::LoadStatus::Loaded) {
    return {.status = BootstrapStatus::Invalid,
            .message = loaded.diagnostics.empty()
                           ? "active profile bootstrap is invalid"
                           : loaded.diagnostics.front()};
  }
  try {
    const std::string id =
        loaded.document.at("activeProfileId").get<std::string>();
    if (!isUuid(id)) {
      return {.status = BootstrapStatus::Invalid,
              .message = "active profile bootstrap contains an invalid UUID"};
    }
    return {.status = BootstrapStatus::Loaded, .id = id};
  } catch (const Json::exception &error) {
    return {.status = BootstrapStatus::Invalid,
            .message = std::string("active profile bootstrap is invalid: ") +
                       error.what()};
  }
}

bool writeProfileMetadata(const std::filesystem::path &path,
                          const PlayerProfile &profile,
                          std::string &errorMessage) {
  return versioned_json::saveAtomic(path, profileJson(profile), errorMessage);
}

bool writeBootstrap(const std::filesystem::path &applicationRoot,
                    std::string_view id, std::string &errorMessage) {
  return versioned_json::saveAtomic(applicationRoot / "active-profile.json",
                                    bootstrapJson(id), errorMessage);
}

bool runPhase(const PlayerProfileManagerDependencies &dependencies,
              ProfileMigrationPhase phase, std::string &errorMessage) {
  return !dependencies.beforeMigrationPhase ||
         dependencies.beforeMigrationPhase(phase, errorMessage);
}

void cleanupStaging(const std::filesystem::path &path,
                    std::string &errorMessage) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) {
    if (!errorMessage.empty()) {
      errorMessage += "; ";
    }
    errorMessage += "unable to clean staging directory: " + error.message();
  }
}

bool compareSourceRows(const std::filesystem::path &source,
                       const std::filesystem::path &destination,
                       std::string &errorMessage) {
  const auto sourceCounts = sqliteUserTableRowCounts(source, errorMessage);
  if (!sourceCounts) {
    return false;
  }
  const auto destinationCounts =
      sqliteUserTableRowCounts(destination, errorMessage);
  if (!destinationCounts) {
    return false;
  }
  for (const auto &[table, count] : *sourceCounts) {
    const auto found = destinationCounts->find(table);
    if (found == destinationCounts->end() || found->second != count) {
      errorMessage = "row count mismatch for SQLite table '" + table + "'";
      return false;
    }
  }
  return true;
}

bool pathExists(const std::filesystem::path &path, bool &exists,
                std::string &errorMessage) {
  std::error_code error;
  exists = std::filesystem::exists(path, error);
  if (error) {
    errorMessage =
        "unable to inspect '" + path.string() + "': " + error.message();
    return false;
  }
  return true;
}

BuildProfileResult buildProfile(
    const std::filesystem::path &applicationRoot,
    const PlayerProfileManagerDependencies &dependencies, std::string id,
    std::string displayName, BuildMode mode,
    const std::optional<PlayerProfilePaths> &duplicateSource = std::nullopt) {
  BuildProfileResult outcome;
  const PlayerProfilePaths staging = makeStagingPaths(applicationRoot, id);
  const PlayerProfilePaths destination = makePaths(applicationRoot, id);
  outcome.paths = destination;
  std::string errorMessage;
  auto fail = [&](ProfileError error, std::string message) {
    cleanupStaging(staging.root, message);
    outcome.result = failure(error, std::move(message));
    return outcome;
  };
  auto migrationPhase = [&](ProfileMigrationPhase phase) {
    return mode != BuildMode::Migration ||
           runPhase(dependencies, phase, errorMessage);
  };

  if (!migrationPhase(ProfileMigrationPhase::PrepareStaging)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  bool destinationExists = false;
  if (!pathExists(destination.root, destinationExists, errorMessage)) {
    return fail(ProfileError::IoFailure, errorMessage);
  }
  if (destinationExists) {
    return fail(ProfileError::IoFailure, "profile UUID already exists");
  }
  cleanupStaging(staging.root, errorMessage);
  if (!errorMessage.empty()) {
    return fail(ProfileError::IoFailure, errorMessage);
  }
  std::error_code filesystemError;
  std::filesystem::create_directories(staging.root, filesystemError);
  if (filesystemError) {
    return fail(ProfileError::IoFailure,
                "unable to create profile staging directory: " +
                    filesystemError.message());
  }

  if (!migrationPhase(ProfileMigrationPhase::WriteSettings)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  if (mode == BuildMode::Duplicate) {
    std::filesystem::copy_file(
        duplicateSource->settingsJson, staging.settingsJson,
        std::filesystem::copy_options::none, filesystemError);
    if (filesystemError) {
      return fail(ProfileError::IoFailure,
                  "unable to duplicate profile settings: " +
                      filesystemError.message());
    }
  } else {
    AppSettings settings;
    if (mode == BuildMode::Migration) {
      bool legacyExists = false;
      if (!pathExists(applicationRoot / "settings.cfg", legacyExists,
                      errorMessage)) {
        return fail(ProfileError::MigrationFailure, errorMessage);
      }
      if (legacyExists) {
        const auto loaded =
            AppSettingsStore::LoadLegacyCfg(applicationRoot / "settings.cfg");
        if (loaded.status != AppSettingsLoadStatus::Loaded) {
          return fail(ProfileError::MigrationFailure,
                      loaded.diagnostics.empty()
                          ? "unable to migrate legacy settings"
                          : loaded.diagnostics.front());
        }
        settings = loaded.settings;
      }
    }
    settings.sanitize();
    if (!AppSettingsStore::Save(staging.settingsJson, settings, errorMessage)) {
      return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                               : ProfileError::IoFailure,
                  errorMessage);
    }
  }

  if (!migrationPhase(ProfileMigrationPhase::WriteInput)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  if (mode == BuildMode::Duplicate) {
    filesystemError.clear();
    std::filesystem::copy_file(duplicateSource->inputJson, staging.inputJson,
                               std::filesystem::copy_options::none,
                               filesystemError);
    if (filesystemError) {
      return fail(ProfileError::IoFailure,
                  "unable to duplicate input profile: " +
                      filesystemError.message());
    }
  } else {
    if (!InputProfileStore::saveAtomic(
            staging.inputJson, makeDefaultInputProfile(), errorMessage)) {
      return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                               : ProfileError::IoFailure,
                  errorMessage);
    }
  }

  const auto snapshot = dependencies.snapshotDatabase
                            ? dependencies.snapshotDatabase
                            : snapshotSqliteDatabase;
  std::optional<std::filesystem::path> scoreSource;
  std::optional<std::filesystem::path> replaySource;
  if (mode == BuildMode::Migration) {
    scoreSource = applicationRoot / "db" / "score.db";
    replaySource = applicationRoot / "db" / "replay.db";
  } else if (mode == BuildMode::Duplicate) {
    scoreSource = duplicateSource->scoresDb;
    replaySource = duplicateSource->replaysDb;
  }

  if (!migrationPhase(ProfileMigrationPhase::SnapshotScores)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  bool hasScoreSource = false;
  if (scoreSource && !pathExists(*scoreSource, hasScoreSource, errorMessage)) {
    return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                             : ProfileError::IoFailure,
                errorMessage);
  }
  if (hasScoreSource &&
      !snapshot(*scoreSource, staging.scoresDb, errorMessage)) {
    return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                             : ProfileError::IoFailure,
                "unable to snapshot score database: " + errorMessage);
  }

  if (!migrationPhase(ProfileMigrationPhase::SnapshotReplays)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  bool hasReplaySource = false;
  if (replaySource &&
      !pathExists(*replaySource, hasReplaySource, errorMessage)) {
    return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                             : ProfileError::IoFailure,
                errorMessage);
  }
  if (hasReplaySource &&
      !snapshot(*replaySource, staging.replaysDb, errorMessage)) {
    return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                             : ProfileError::IoFailure,
                "unable to snapshot replay database: " + errorMessage);
  }

  if (!migrationPhase(ProfileMigrationPhase::EnsureScoreSchema)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  if (!app_database_initializer::initializeScoreDatabase(staging.scoresDb)) {
    return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                             : ProfileError::IntegrityFailure,
                "unable to initialize profile score database");
  }

  if (!migrationPhase(ProfileMigrationPhase::EnsureReplaySchema)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  if (!app_database_initializer::initializeReplayDatabase(staging.replaysDb)) {
    return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                             : ProfileError::IntegrityFailure,
                "unable to initialize profile replay database");
  }

  if (!migrationPhase(ProfileMigrationPhase::ValidateIntegrity)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  if (!sqliteIntegrityCheck(staging.scoresDb, errorMessage) ||
      !sqliteIntegrityCheck(staging.replaysDb, errorMessage)) {
    return fail(ProfileError::IntegrityFailure, errorMessage);
  }

  if (!migrationPhase(ProfileMigrationPhase::CompareRows)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  if ((hasScoreSource &&
       !compareSourceRows(*scoreSource, staging.scoresDb, errorMessage)) ||
      (hasReplaySource &&
       !compareSourceRows(*replaySource, staging.replaysDb, errorMessage))) {
    return fail(ProfileError::IntegrityFailure, errorMessage);
  }

  if (!migrationPhase(ProfileMigrationPhase::WriteMetadata)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  const std::string timestamp = dependencies.utcNow();
  PlayerProfile profile{.schemaVersion = kPlayerProfileSchemaVersion,
                        .id = std::move(id),
                        .displayName = std::move(displayName),
                        .createdAt = timestamp,
                        .lastUsedAt = timestamp};
  if (!writeProfileMetadata(staging.profileJson, profile, errorMessage)) {
    return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                             : ProfileError::IoFailure,
                errorMessage);
  }

  if (!migrationPhase(ProfileMigrationPhase::FinalizeProfile)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  std::filesystem::rename(staging.root, destination.root, filesystemError);
  if (filesystemError) {
    return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                             : ProfileError::IoFailure,
                "unable to finalize profile: " + filesystemError.message());
  }
  outcome.finalized = true;
  outcome.result = success(std::move(profile));
  return outcome;
}

bool cleanupAbandonedStaging(const std::filesystem::path &applicationRoot,
                             std::string &errorMessage) {
  const auto profiles = applicationRoot / "profiles";
  std::error_code error;
  std::filesystem::create_directories(profiles, error);
  if (error) {
    errorMessage = "unable to create profiles directory: " + error.message();
    return false;
  }
  std::filesystem::directory_iterator iterator(profiles, error);
  if (error) {
    errorMessage = "unable to enumerate profiles directory: " + error.message();
    return false;
  }
  for (const auto &entry : iterator) {
    if (!entry.path().filename().string().starts_with(".staging-")) {
      continue;
    }
    std::filesystem::remove_all(entry.path(), error);
    if (error) {
      errorMessage = "unable to remove abandoned profile staging directory: " +
                     error.message();
      return false;
    }
  }
  return true;
}
} // namespace

PlayerProfileManager::PlayerProfileManager(
    std::filesystem::path applicationDataRoot,
    PlayerProfileManagerDependencies dependencies)
    : applicationDataRoot_(std::move(applicationDataRoot)),
      dependencies_(std::move(dependencies)) {
  if (!dependencies_.generateUuid) {
    dependencies_.generateUuid = defaultUuid;
  }
  if (!dependencies_.utcNow) {
    dependencies_.utcNow = defaultUtcNow;
  }
  if (!dependencies_.snapshotDatabase) {
    dependencies_.snapshotDatabase = snapshotSqliteDatabase;
  }
}

ProfileResult PlayerProfileManager::Initialize() {
  activeProfile_.reset();
  std::string errorMessage;
  if (!cleanupAbandonedStaging(applicationDataRoot_, errorMessage)) {
    return failure(ProfileError::IoFailure, errorMessage);
  }

  const auto bootstrapPath = applicationDataRoot_ / "active-profile.json";
  const auto bootstrapBackup =
      std::filesystem::path(bootstrapPath.string() + ".bak");
  const BootstrapResult bootstrap = loadBootstrap(bootstrapPath);
  if (bootstrap.status == BootstrapStatus::FutureVersion) {
    return failure(ProfileError::FutureVersion, bootstrap.message);
  }
  if (bootstrap.status == BootstrapStatus::Loaded) {
    const ProfileResult validated = validateProfile(bootstrap.id);
    if (validated.ok()) {
      activeProfile_ = validated.profile;
      return validated;
    }
    if (validated.error == ProfileError::FutureVersion) {
      return validated;
    }
  }

  const BootstrapResult backup = loadBootstrap(bootstrapBackup);
  if (backup.status == BootstrapStatus::FutureVersion) {
    return failure(ProfileError::FutureVersion, backup.message);
  }
  if (backup.status == BootstrapStatus::Loaded) {
    const ProfileResult validated = validateProfile(backup.id);
    if (validated.ok()) {
      if (!writeBootstrap(applicationDataRoot_, backup.id, errorMessage)) {
        return failure(ProfileError::IoFailure,
                       "unable to recover active profile bootstrap: " +
                           errorMessage);
      }
      activeProfile_ = validated.profile;
      return validated;
    }
    if (validated.error == ProfileError::FutureVersion) {
      return validated;
    }
  }

  std::vector<PlayerProfile> profiles = listProfiles();
  if (!profiles.empty()) {
    const PlayerProfile recovered = profiles.front();
    if (!writeBootstrap(applicationDataRoot_, recovered.id, errorMessage)) {
      return failure(ProfileError::IoFailure,
                     "unable to recover orphan profile bootstrap: " +
                         errorMessage);
    }
    activeProfile_ = recovered;
    return success(recovered);
  }

  const auto profilesRoot = applicationDataRoot_ / "profiles";
  std::error_code scanError;
  std::filesystem::directory_iterator profileIterator(profilesRoot, scanError);
  if (scanError) {
    return failure(ProfileError::IoFailure,
                   "unable to inspect finalized profiles: " +
                       scanError.message());
  }
  for (const auto &entry : profileIterator) {
    const std::string candidateId = entry.path().filename().string();
    if (!isUuid(candidateId)) {
      continue;
    }
    const ProfileResult candidate = validateProfile(candidateId);
    if (candidate.error == ProfileError::FutureVersion) {
      return candidate;
    }
  }

  std::string id;
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::string candidate = dependencies_.generateUuid();
    if (!isUuid(candidate)) {
      continue;
    }
    bool exists = false;
    if (!pathExists(makePaths(applicationDataRoot_, candidate).root, exists,
                    errorMessage)) {
      return failure(ProfileError::IoFailure, errorMessage);
    }
    if (!exists) {
      id = candidate;
      break;
    }
  }
  if (id.empty()) {
    return failure(ProfileError::MigrationFailure,
                   "unable to generate a unique profile UUID");
  }

  BuildProfileResult built = buildProfile(applicationDataRoot_, dependencies_,
                                          id, "Player", BuildMode::Migration);
  if (!built.result.ok()) {
    return built.result;
  }
  if (!runPhase(dependencies_, ProfileMigrationPhase::WriteBootstrap,
                errorMessage)) {
    return failure(ProfileError::MigrationFailure, errorMessage);
  }
  if (!writeBootstrap(applicationDataRoot_, id, errorMessage)) {
    return failure(ProfileError::MigrationFailure,
                   "unable to write active profile bootstrap: " + errorMessage);
  }
  activeProfile_ = built.result.profile;
  return built.result;
}

const PlayerProfile &PlayerProfileManager::activeProfile() const {
  if (!activeProfile_) {
    throw std::logic_error("PlayerProfileManager is not initialized");
  }
  return *activeProfile_;
}

PlayerProfilePaths PlayerProfileManager::activePaths() const {
  return activeProfile_ ? pathsFor(activeProfile_->id) : PlayerProfilePaths{};
}

PlayerProfilePaths PlayerProfileManager::pathsFor(std::string_view id) const {
  return makePaths(applicationDataRoot_, id);
}

std::vector<PlayerProfile> PlayerProfileManager::listProfiles() const {
  std::vector<PlayerProfile> profiles;
  const auto root = applicationDataRoot_ / "profiles";
  std::error_code error;
  std::filesystem::directory_iterator iterator(root, error);
  if (error) {
    return profiles;
  }
  for (const auto &entry : iterator) {
    const std::string id = entry.path().filename().string();
    if (!isUuid(id)) {
      continue;
    }
    const ProfileResult validated = validateProfile(id);
    if (validated.ok() && validated.profile) {
      profiles.push_back(*validated.profile);
    }
  }
  std::ranges::sort(profiles,
                    [](const PlayerProfile &left, const PlayerProfile &right) {
                      if (left.lastUsedAt != right.lastUsedAt) {
                        return left.lastUsedAt > right.lastUsedAt;
                      }
                      return left.id < right.id;
                    });
  return profiles;
}

ProfileResult PlayerProfileManager::createProfile(std::string displayName) {
  if (!activeProfile_) {
    return failure(ProfileError::SwitchBlocked,
                   "profile manager is not initialized");
  }
  const auto name = normalizedName(displayName);
  if (!name) {
    return failure(ProfileError::InvalidName, "profile name is invalid");
  }
  std::string id;
  std::string errorMessage;
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::string candidate = dependencies_.generateUuid();
    bool exists = false;
    if (isUuid(candidate) &&
        pathExists(pathsFor(candidate).root, exists, errorMessage) && !exists) {
      id = candidate;
      break;
    }
  }
  if (id.empty()) {
    return failure(ProfileError::IoFailure,
                   errorMessage.empty()
                       ? "unable to generate a unique profile UUID"
                       : errorMessage);
  }
  return buildProfile(applicationDataRoot_, dependencies_, id, *name,
                      BuildMode::Create)
      .result;
}

ProfileResult PlayerProfileManager::duplicateProfile(std::string_view sourceId,
                                                     std::string displayName) {
  if (!activeProfile_) {
    return failure(ProfileError::SwitchBlocked,
                   "profile manager is not initialized");
  }
  const ProfileResult source = validateProfile(sourceId);
  if (!source.ok()) {
    return source;
  }
  const auto name = normalizedName(displayName);
  if (!name) {
    return failure(ProfileError::InvalidName, "profile name is invalid");
  }
  std::string id;
  std::string errorMessage;
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::string candidate = dependencies_.generateUuid();
    bool exists = false;
    if (isUuid(candidate) &&
        pathExists(pathsFor(candidate).root, exists, errorMessage) && !exists) {
      id = candidate;
      break;
    }
  }
  if (id.empty()) {
    return failure(ProfileError::IoFailure,
                   errorMessage.empty()
                       ? "unable to generate a unique profile UUID"
                       : errorMessage);
  }
  return buildProfile(applicationDataRoot_, dependencies_, id, *name,
                      BuildMode::Duplicate, pathsFor(sourceId))
      .result;
}

ProfileResult PlayerProfileManager::renameProfile(std::string_view id,
                                                  std::string displayName) {
  const auto name = normalizedName(displayName);
  if (!name) {
    return failure(ProfileError::InvalidName, "profile name is invalid");
  }
  ProfileResult validated = validateProfile(id);
  if (!validated.ok() || !validated.profile) {
    return validated;
  }
  PlayerProfile updated = *validated.profile;
  updated.displayName = *name;
  std::string errorMessage;
  if (!writeProfileMetadata(pathsFor(id).profileJson, updated, errorMessage)) {
    return failure(ProfileError::IoFailure, errorMessage);
  }
  if (activeProfile_ && activeProfile_->id == id) {
    activeProfile_ = updated;
  }
  return success(std::move(updated));
}

ProfileResult PlayerProfileManager::deleteProfile(std::string_view id) {
  if (!activeProfile_) {
    return failure(ProfileError::SwitchBlocked,
                   "profile manager is not initialized");
  }
  const ProfileResult validated = validateProfile(id);
  if (!validated.ok()) {
    return validated;
  }
  const auto profiles = listProfiles();
  if (profiles.size() <= 1) {
    return failure(ProfileError::LastProfileDeletion,
                   "the last profile cannot be deleted");
  }
  if (activeProfile_->id == id) {
    return failure(ProfileError::ActiveProfileDeletion,
                   "the active profile cannot be deleted");
  }

  const auto source = pathsFor(id).root;
  const auto tombstone =
      applicationDataRoot_ / "profiles" / (".deleting-" + std::string(id));
  std::error_code error;
  std::filesystem::remove_all(tombstone, error);
  error.clear();
  std::filesystem::rename(source, tombstone, error);
  if (error) {
    return failure(ProfileError::IoFailure,
                   "unable to stage profile deletion: " + error.message());
  }
  std::filesystem::remove_all(tombstone, error);
  if (error) {
    std::error_code restoreError;
    std::filesystem::rename(tombstone, source, restoreError);
    std::string message = "unable to delete profile data: " + error.message();
    if (restoreError) {
      message += "; unable to restore profile: " + restoreError.message();
    }
    return failure(ProfileError::IoFailure, std::move(message));
  }
  return success(*validated.profile);
}

ProfileResult PlayerProfileManager::validateProfile(std::string_view id) const {
  if (!isUuid(id)) {
    return failure(ProfileError::NotFound, "profile UUID is invalid");
  }
  const PlayerProfilePaths paths = pathsFor(id);
  std::error_code error;
  const auto rootStatus = std::filesystem::symlink_status(paths.root, error);
  if (error || !std::filesystem::is_directory(rootStatus)) {
    return failure(ProfileError::NotFound, "profile directory is missing");
  }
  error.clear();
  const auto metadataStatus =
      std::filesystem::symlink_status(paths.profileJson, error);
  if (error || !std::filesystem::is_regular_file(metadataStatus)) {
    return failure(ProfileError::IntegrityFailure,
                   "profile metadata is missing");
  }
  ProfileResult metadata = loadProfileMetadata(paths.profileJson, id);
  if (!metadata.ok()) {
    return metadata;
  }
  for (const auto &file :
       {paths.settingsJson, paths.inputJson, paths.scoresDb, paths.replaysDb}) {
    error.clear();
    const auto status = std::filesystem::symlink_status(file, error);
    if (error || !std::filesystem::is_regular_file(status)) {
      return failure(ProfileError::IntegrityFailure,
                     "profile component is missing: " +
                         file.filename().string());
    }
  }

  const auto settings = AppSettingsStore::Load(paths.settingsJson);
  if (settings.status == AppSettingsLoadStatus::FutureVersion) {
    return failure(ProfileError::FutureVersion,
                   "profile settings are newer than supported");
  }
  if (settings.status != AppSettingsLoadStatus::Loaded) {
    return failure(ProfileError::IntegrityFailure,
                   "profile settings are invalid");
  }
  const auto input = InputProfileStore::load(paths.inputJson);
  if (input.status == InputProfileLoadStatus::FutureVersion) {
    return failure(ProfileError::FutureVersion,
                   "profile input data is newer than supported");
  }
  if (input.status != InputProfileLoadStatus::Loaded) {
    return failure(ProfileError::IntegrityFailure,
                   "profile input data is invalid");
  }

  std::string errorMessage;
  const auto scoreVersion =
      sqliteDatabaseUserVersion(paths.scoresDb, errorMessage);
  const auto replayVersion =
      sqliteDatabaseUserVersion(paths.replaysDb, errorMessage);
  if (!scoreVersion || !replayVersion) {
    return failure(ProfileError::IntegrityFailure, errorMessage);
  }
  if (*scoreVersion > ScoreDBHelper::kCurrentSchemaVersion ||
      *replayVersion > ReplayDBHelper::kCurrentSchemaVersion) {
    return failure(ProfileError::FutureVersion,
                   "profile database is newer than supported");
  }
  if (*scoreVersion != ScoreDBHelper::kCurrentSchemaVersion ||
      *replayVersion != ReplayDBHelper::kCurrentSchemaVersion) {
    return failure(ProfileError::IntegrityFailure,
                   "profile database schema is not current");
  }
  if (!sqliteIntegrityCheck(paths.scoresDb, errorMessage) ||
      !sqliteIntegrityCheck(paths.replaysDb, errorMessage)) {
    return failure(ProfileError::IntegrityFailure, errorMessage);
  }
  return metadata;
}

ProfileResult PlayerProfileManager::commitActiveProfile(std::string_view id) {
  if (!activeProfile_) {
    return failure(ProfileError::SwitchBlocked,
                   "profile manager is not initialized");
  }
  ProfileResult validated = validateProfile(id);
  if (!validated.ok() || !validated.profile) {
    return validated;
  }
  if (activeProfile_->id == id) {
    return validated;
  }

  const PlayerProfile previousMetadata = *validated.profile;
  PlayerProfile updated = previousMetadata;
  updated.lastUsedAt = dependencies_.utcNow();
  std::string errorMessage;
  if (!writeProfileMetadata(pathsFor(id).profileJson, updated, errorMessage)) {
    return failure(ProfileError::IoFailure,
                   "unable to update profile metadata: " + errorMessage);
  }
  if (!writeBootstrap(applicationDataRoot_, id, errorMessage)) {
    std::string restoreError;
    if (!writeProfileMetadata(pathsFor(id).profileJson, previousMetadata,
                              restoreError)) {
      errorMessage += "; unable to restore profile metadata: " + restoreError;
    }
    return failure(ProfileError::IoFailure,
                   "unable to switch active profile: " + errorMessage);
  }
  activeProfile_ = updated;
  return success(std::move(updated));
}
