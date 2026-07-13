#include "PlayerProfileManager.h"

#include "AppDatabaseInitializer.h"
#include "AppSettingsStore.h"
#include "AtomicFile.h"
#include "ProfileDatabaseTools.h"
#include "ReplayDBHelper.h"
#include "ScoreDBHelper.h"
#include "VersionedJson.h"
#include "input/InputProfileStore.h"
#include "practice/PracticePresetStore.h"

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

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
using Json = nlohmann::json;
constexpr std::uintmax_t kMaximumPracticeFileBytes = 1U * 1024U * 1024U;

enum class BuildMode { Migration, Create, Duplicate };

ProfileResult failure(ProfileError error, std::string message) {
  return {
      .error = error, .message = std::move(message), .profile = std::nullopt};
}

ProfileResult success(PlayerProfile profile, std::string message = {}) {
  return {.error = ProfileError::None,
          .message = std::move(message),
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

bool isReparsePoint(const std::filesystem::path &path,
                    std::string &errorMessage) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return false;
    }
    errorMessage = "unable to inspect path attributes for '" + path.string() +
                   "': " + std::to_string(error);
    return true;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  (void)path;
  (void)errorMessage;
  return false;
#endif
}

bool hasUnsafeLink(const std::filesystem::path &path,
                   const std::filesystem::file_status &status,
                   std::string &errorMessage) {
  return std::filesystem::is_symlink(status) ||
         isReparsePoint(path, errorMessage);
}

bool inspectDirectoryWithoutLinks(const std::filesystem::path &path,
                                  bool allowMissing,
                                  std::string &errorMessage) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error) {
    if (allowMissing &&
        error == std::make_error_code(std::errc::no_such_file_or_directory)) {
      return true;
    }
    errorMessage = "unable to inspect directory '" + path.string() +
                   "': " + error.message();
    return false;
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    if (allowMissing) {
      return true;
    }
    errorMessage = "directory is missing: " + path.string();
    return false;
  }
  if (hasUnsafeLink(path, status, errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage =
          "refusing symlink or reparse-point directory: " + path.string();
    }
    return false;
  }
  if (!std::filesystem::is_directory(status)) {
    errorMessage = "expected a directory at: " + path.string();
    return false;
  }
  return true;
}

bool ensureSafeProfilesRoot(const std::filesystem::path &applicationRoot,
                            bool create, std::string &errorMessage) {
  errorMessage.clear();
  if (!inspectDirectoryWithoutLinks(applicationRoot, create, errorMessage)) {
    return false;
  }
  std::error_code error;
  if (!std::filesystem::exists(applicationRoot, error)) {
    if (error) {
      errorMessage =
          "unable to inspect application data root: " + error.message();
      return false;
    }
    if (!create) {
      errorMessage = "application data root is missing";
      return false;
    }
    std::filesystem::create_directories(applicationRoot, error);
    if (error) {
      errorMessage =
          "unable to create application data root: " + error.message();
      return false;
    }
    if (!inspectDirectoryWithoutLinks(applicationRoot, false, errorMessage)) {
      return false;
    }
  }

  const auto profilesRoot = applicationRoot / "profiles";
  if (!inspectDirectoryWithoutLinks(profilesRoot, create, errorMessage)) {
    return false;
  }
  if (!std::filesystem::exists(profilesRoot, error)) {
    if (error) {
      errorMessage = "unable to inspect profiles root: " + error.message();
      return false;
    }
    if (!create) {
      errorMessage = "profiles root is missing";
      return false;
    }
    std::filesystem::create_directory(profilesRoot, error);
    if (error) {
      errorMessage = "unable to create profiles directory: " + error.message();
      return false;
    }
    if (!inspectDirectoryWithoutLinks(profilesRoot, false, errorMessage)) {
      return false;
    }
  }

  const auto resolvedApplicationRoot =
      std::filesystem::canonical(applicationRoot, error);
  if (error) {
    errorMessage =
        "unable to resolve application data root: " + error.message();
    return false;
  }
  const auto resolvedProfilesRoot =
      std::filesystem::canonical(profilesRoot, error);
  if (error) {
    errorMessage = "unable to resolve profiles root: " + error.message();
    return false;
  }
  if (resolvedProfilesRoot.parent_path() != resolvedApplicationRoot) {
    errorMessage = "resolved profiles root escapes the application data root";
    return false;
  }
  return true;
}

bool pathHasPrefix(const std::filesystem::path &path,
                   const std::filesystem::path &prefix) {
  auto pathPart = path.begin();
  for (auto prefixPart = prefix.begin(); prefixPart != prefix.end();
       ++prefixPart, ++pathPart) {
    if (pathPart == path.end() || *pathPart != *prefixPart) {
      return false;
    }
  }
  return true;
}

bool ensureContainedPath(const std::filesystem::path &applicationRoot,
                         const std::filesystem::path &candidate,
                         std::string &errorMessage) {
  if (!ensureSafeProfilesRoot(applicationRoot, false, errorMessage)) {
    return false;
  }
  std::error_code error;
  const auto profilesRoot = applicationRoot / "profiles";
  const auto resolvedProfilesRoot =
      std::filesystem::canonical(profilesRoot, error);
  if (error) {
    errorMessage = "unable to resolve profiles root: " + error.message();
    return false;
  }
  const auto resolvedCandidate =
      std::filesystem::weakly_canonical(candidate, error);
  if (error) {
    errorMessage = "unable to resolve profile path '" + candidate.string() +
                   "': " + error.message();
    return false;
  }
  if (resolvedCandidate == resolvedProfilesRoot ||
      !pathHasPrefix(resolvedCandidate, resolvedProfilesRoot)) {
    errorMessage =
        "profile path escapes the profiles root: " + candidate.string();
    return false;
  }

  const auto relative = candidate.lexically_normal().lexically_relative(
      profilesRoot.lexically_normal());
  if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
    errorMessage =
        "profile path is not beneath the profiles root: " + candidate.string();
    return false;
  }
  std::filesystem::path current = profilesRoot;
  for (const auto &part : relative) {
    current /= part;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error) {
      if (error == std::make_error_code(std::errc::no_such_file_or_directory)) {
        error.clear();
        continue;
      }
      errorMessage = "unable to inspect profile path '" + current.string() +
                     "': " + error.message();
      return false;
    }
    if (status.type() == std::filesystem::file_type::not_found) {
      continue;
    }
    if (hasUnsafeLink(current, status, errorMessage)) {
      if (errorMessage.empty()) {
        errorMessage = "refusing symlink or reparse point in profile path: " +
                       current.string();
      }
      return false;
    }
  }
  return true;
}

PlayerProfilePaths makePathsAtRoot(const std::filesystem::path &root) {
  PlayerProfilePaths paths;
  paths.root = root;
  paths.profileJson = paths.root / "profile.json";
  paths.settingsJson = paths.root / "settings.json";
  paths.inputJson = paths.root / "input.json";
  paths.scoresDb = paths.root / "scores.db";
  paths.replaysDb = paths.root / "replays.db";
  paths.practiceDirectory = paths.root / "practice";
  return paths;
}

bool validatePracticeDirectory(const std::filesystem::path &applicationRoot,
                               const PlayerProfilePaths &paths,
                               std::vector<std::filesystem::path> *files,
                               std::string &errorMessage) {
  if (!ensureContainedPath(applicationRoot, paths.practiceDirectory,
                           errorMessage)) {
    return false;
  }
  std::error_code error;
  const auto status =
      std::filesystem::symlink_status(paths.practiceDirectory, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return true;
    }
    errorMessage =
        "unable to inspect profile practice directory: " + error.message();
    return false;
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    return true;
  }
  if (!std::filesystem::is_directory(status) ||
      hasUnsafeLink(paths.practiceDirectory, status, errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage = "profile practice path is not a safe directory";
    }
    return false;
  }

  std::filesystem::directory_iterator iterator(paths.practiceDirectory, error);
  if (error) {
    errorMessage =
        "unable to enumerate profile practice directory: " + error.message();
    return false;
  }
  for (const auto &entry : iterator) {
    if (!ensureContainedPath(applicationRoot, entry.path(), errorMessage)) {
      return false;
    }
    error.clear();
    const auto entryStatus =
        std::filesystem::symlink_status(entry.path(), error);
    const std::string filename = entry.path().filename().string();
    const practice::PresetFileKind kind =
        practice::classifyPresetFilename(filename);
    const bool primary = kind == practice::PresetFileKind::Primary;
    if (error || !std::filesystem::is_regular_file(entryStatus) ||
        hasUnsafeLink(entry.path(), entryStatus, errorMessage) ||
        kind == practice::PresetFileKind::Invalid) {
      if (errorMessage.empty()) {
        errorMessage =
            "profile practice directory contains an unsafe or invalid entry";
      }
      return false;
    }
    const auto size = std::filesystem::file_size(entry.path(), error);
    if (error || size > kMaximumPracticeFileBytes) {
      errorMessage = error ? "unable to inspect profile practice file size: " +
                                 error.message()
                           : "profile practice file exceeds the 1 MiB limit";
      return false;
    }
    if (files && primary) {
      files->push_back(entry.path());
    }
  }
  if (files) {
    std::ranges::sort(*files);
  }
  return true;
}

bool copyPracticeDirectory(const std::filesystem::path &applicationRoot,
                           const PlayerProfilePaths &source,
                           const PlayerProfilePaths &destination,
                           std::string &errorMessage) {
  std::vector<std::filesystem::path> files;
  if (!validatePracticeDirectory(applicationRoot, source, &files,
                                 errorMessage)) {
    return false;
  }
  std::error_code error;
  if (!std::filesystem::create_directory(destination.practiceDirectory,
                                         error) &&
      (error ||
       !std::filesystem::is_directory(destination.practiceDirectory))) {
    errorMessage = "unable to create duplicate practice directory: " +
                   (error ? error.message() : "path already exists");
    return false;
  }
  for (const auto &sourceFile : files) {
    std::filesystem::copy_file(
        sourceFile, destination.practiceDirectory / sourceFile.filename(),
        std::filesystem::copy_options::none, error);
    if (error) {
      errorMessage = "unable to copy practice preset: " + error.message();
      return false;
    }
  }
  return true;
}

PlayerProfilePaths makePaths(const std::filesystem::path &applicationRoot,
                             std::string_view id) {
  if (!isUuid(id)) {
    return {};
  }
  return makePathsAtRoot(applicationRoot / "profiles" / std::string(id));
}

PlayerProfilePaths
makeStagingPaths(const std::filesystem::path &applicationRoot,
                 std::string_view id) {
  return makePathsAtRoot(applicationRoot / "profiles" /
                         (".staging-" + std::string(id)));
}

std::filesystem::path
makeBackupPath(const std::filesystem::path &applicationRoot,
               std::string_view id) {
  return applicationRoot / "profiles" / (".backup-" + std::string(id));
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

bool removeTree(const std::filesystem::path &path, std::string &errorMessage) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) {
    errorMessage =
        "unable to remove '" + path.string() + "': " + error.message();
    return false;
  }
  return true;
}

bool cleanupStaging(const std::filesystem::path &applicationRoot,
                    const PlayerProfileManagerDependencies &dependencies,
                    const std::filesystem::path &path,
                    std::string &errorMessage) {
  std::string cleanupError;
  if (!ensureContainedPath(applicationRoot, path, cleanupError) ||
      !dependencies.filesystem.removeTree(path, cleanupError)) {
    if (!errorMessage.empty()) {
      errorMessage += "; ";
    }
    errorMessage += "unable to clean staging directory: " + cleanupError;
    return false;
  }
  return true;
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

ProfileResult validateProfileFiles(const std::filesystem::path &applicationRoot,
                                   const PlayerProfilePaths &paths,
                                   std::string_view expectedId,
                                   bool allowSupportedOlderDatabases) {
  std::string safetyError;
  if (!ensureSafeProfilesRoot(applicationRoot, false, safetyError) ||
      !ensureContainedPath(applicationRoot, paths.root, safetyError) ||
      !ensureContainedPath(applicationRoot, paths.profileJson, safetyError)) {
    return failure(ProfileError::IoFailure, safetyError);
  }
  std::error_code error;
  const auto rootStatus = std::filesystem::symlink_status(paths.root, error);
  if (error || !std::filesystem::is_directory(rootStatus) ||
      hasUnsafeLink(paths.root, rootStatus, safetyError)) {
    return failure(ProfileError::IntegrityFailure,
                   "profile directory is not a safe directory");
  }
  for (const auto &file : {paths.profileJson, paths.settingsJson,
                           paths.inputJson, paths.scoresDb, paths.replaysDb}) {
    if (!ensureContainedPath(applicationRoot, file, safetyError)) {
      return failure(ProfileError::IntegrityFailure, safetyError);
    }
    error.clear();
    const auto status = std::filesystem::symlink_status(file, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        hasUnsafeLink(file, status, safetyError)) {
      return failure(ProfileError::IntegrityFailure,
                     "profile component is not a safe regular file: " +
                         file.filename().string());
    }
  }
  if (!validatePracticeDirectory(applicationRoot, paths, nullptr,
                                 safetyError)) {
    return failure(ProfileError::IntegrityFailure, safetyError);
  }

  ProfileResult metadata = loadProfileMetadata(paths.profileJson, expectedId);
  if (!metadata.ok()) {
    return metadata;
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
  if (!allowSupportedOlderDatabases &&
      (*scoreVersion != ScoreDBHelper::kCurrentSchemaVersion ||
       *replayVersion != ReplayDBHelper::kCurrentSchemaVersion)) {
    return failure(ProfileError::IntegrityFailure,
                   "profile database schema is not current");
  }
  if (!sqliteIntegrityCheck(paths.scoresDb, errorMessage) ||
      !sqliteIntegrityCheck(paths.replaysDb, errorMessage)) {
    return failure(ProfileError::IntegrityFailure, errorMessage);
  }
  return metadata;
}

ProfileResult finalizeNewProfileDirectory(
    const std::filesystem::path &applicationRoot,
    const PlayerProfileManagerDependencies &dependencies,
    const PlayerProfilePaths &staging, const PlayerProfilePaths &destination,
    PlayerProfile profile) {
  struct DirectoryState {
    bool stagingExists = false;
    bool destinationExists = false;
    std::optional<ProfileResult> destinationValidation;
  };

  const auto profilesRoot = applicationRoot / "profiles";
  auto appendDetail = [](std::string &message, std::string detail) {
    if (detail.empty()) {
      return;
    }
    if (!message.empty()) {
      message += "; ";
    }
    message += std::move(detail);
  };
  auto inspectState = [&](DirectoryState &state, std::string &inspectionError) {
    if (!ensureContainedPath(applicationRoot, staging.root, inspectionError) ||
        !ensureContainedPath(applicationRoot, destination.root,
                             inspectionError) ||
        !pathExists(staging.root, state.stagingExists, inspectionError) ||
        !pathExists(destination.root, state.destinationExists,
                    inspectionError)) {
      return false;
    }
    if (state.destinationExists) {
      state.destinationValidation =
          validateProfileFiles(applicationRoot, destination, profile.id, false);
    }
    return true;
  };
  auto invalidDestinationFailure = [&](const DirectoryState &state) {
    const ProfileResult &validation = *state.destinationValidation;
    return failure(
        validation.error == ProfileError::None ? ProfileError::IntegrityFailure
                                               : validation.error,
        "profile destination is present but cannot be safely accepted: " +
            validation.message);
  };
  auto installedResult = [&](const DirectoryState &state) {
    std::string warning;
    if (state.stagingExists) {
      std::string cleanupError;
      if (!cleanupStaging(applicationRoot, dependencies, staging.root,
                          cleanupError)) {
        appendDetail(warning, "profile is installed, but " + cleanupError);
      }
    }
    std::string syncError;
    if (!dependencies.filesystem.syncDirectory(profilesRoot, syncError)) {
      appendDetail(warning,
                   "profile is installed, but directory sync should be "
                   "retried: " +
                       syncError);
    }
    return success(std::move(profile), std::move(warning));
  };
  auto failedWithoutDestination = [&](const DirectoryState &state,
                                      std::string message) {
    if (state.stagingExists) {
      const bool cleaned =
          cleanupStaging(applicationRoot, dependencies, staging.root, message);
      if (cleaned) {
        std::string syncError;
        if (!dependencies.filesystem.syncDirectory(profilesRoot, syncError)) {
          appendDetail(message, "unable to sync staging cleanup: " + syncError);
        }
      }
    }
    return failure(ProfileError::IoFailure, std::move(message));
  };

  std::string errorMessage;
  if (!ensureContainedPath(applicationRoot, staging.root, errorMessage) ||
      !ensureContainedPath(applicationRoot, destination.root, errorMessage)) {
    return failure(ProfileError::IoFailure,
                   "unable to inspect profile finalization paths: " +
                       errorMessage);
  }

  const bool renameReportedSuccess = dependencies.filesystem.durableRename(
      staging.root, destination.root, errorMessage);
  const std::string renameError = errorMessage;
  DirectoryState afterRename;
  std::string inspectionError;
  if (!inspectState(afterRename, inspectionError)) {
    return failure(ProfileError::IoFailure,
                   "unable to inspect profile finalization outcome: " +
                       inspectionError);
  }
  if (afterRename.destinationExists &&
      !afterRename.destinationValidation->ok()) {
    return invalidDestinationFailure(afterRename);
  }
  if (!afterRename.destinationExists) {
    std::string message = renameReportedSuccess
                              ? "profile finalization left no destination"
                              : "unable to finalize profile: " + renameError;
    return failedWithoutDestination(afterRename, std::move(message));
  }
  if (!renameReportedSuccess || afterRename.stagingExists) {
    return installedResult(afterRename);
  }

  errorMessage.clear();
  if (dependencies.filesystem.syncDirectory(profilesRoot, errorMessage)) {
    return success(std::move(profile));
  }

  std::string failureMessage =
      "unable to sync finalized profile directory: " + errorMessage;
  std::string rollbackError;
  const bool rollbackReportedSuccess = dependencies.filesystem.durableRename(
      destination.root, staging.root, rollbackError);
  DirectoryState afterRollback;
  inspectionError.clear();
  if (!inspectState(afterRollback, inspectionError)) {
    return failure(
        ProfileError::IoFailure,
        failureMessage +
            "; unable to inspect profile rollback outcome: " + inspectionError);
  }
  if (afterRollback.destinationExists &&
      !afterRollback.destinationValidation->ok()) {
    return invalidDestinationFailure(afterRollback);
  }
  if (afterRollback.destinationExists) {
    return installedResult(afterRollback);
  }

  if (!rollbackReportedSuccess && !rollbackError.empty()) {
    appendDetail(failureMessage,
                 "profile rollback reported failure: " + rollbackError);
  }
  std::string rollbackSyncError;
  if (!dependencies.filesystem.syncDirectory(profilesRoot, rollbackSyncError)) {
    appendDetail(failureMessage,
                 "unable to sync profile rollback: " + rollbackSyncError);
    return failure(ProfileError::IoFailure, std::move(failureMessage));
  }
  if (afterRollback.stagingExists &&
      !cleanupStaging(applicationRoot, dependencies, staging.root,
                      failureMessage)) {
    return failure(ProfileError::IoFailure, std::move(failureMessage));
  }
  if (afterRollback.stagingExists) {
    std::string cleanupSyncError;
    if (!dependencies.filesystem.syncDirectory(profilesRoot,
                                               cleanupSyncError)) {
      appendDetail(failureMessage, "unable to sync profile rollback cleanup: " +
                                       cleanupSyncError);
    }
  }
  return failure(ProfileError::IoFailure, std::move(failureMessage));
}

ProfileResult
finalizeProfileDeletion(const std::filesystem::path &applicationRoot,
                        const PlayerProfileManagerDependencies &dependencies,
                        const PlayerProfilePaths &source,
                        const std::filesystem::path &tombstone,
                        PlayerProfile profile) {
  struct DirectoryState {
    bool sourceExists = false;
    bool tombstoneExists = false;
    std::optional<ProfileResult> sourceValidation;
    std::optional<ProfileResult> tombstoneValidation;
  };

  const auto profilesRoot = applicationRoot / "profiles";
  const PlayerProfilePaths tombstonePaths = makePathsAtRoot(tombstone);
  auto appendDetail = [](std::string &message, std::string detail) {
    if (detail.empty()) {
      return;
    }
    if (!message.empty()) {
      message += "; ";
    }
    message += std::move(detail);
  };
  auto inspectState = [&](DirectoryState &state, std::string &inspectionError) {
    if (!ensureContainedPath(applicationRoot, source.root, inspectionError) ||
        !ensureContainedPath(applicationRoot, tombstone, inspectionError) ||
        !pathExists(source.root, state.sourceExists, inspectionError) ||
        !pathExists(tombstone, state.tombstoneExists, inspectionError)) {
      return false;
    }
    if (state.sourceExists) {
      state.sourceValidation =
          validateProfileFiles(applicationRoot, source, profile.id, false);
    }
    if (state.tombstoneExists) {
      state.tombstoneValidation = validateProfileFiles(
          applicationRoot, tombstonePaths, profile.id, false);
    }
    return true;
  };
  auto sourceRetainedFailure = [&](const DirectoryState &state,
                                   std::string message) {
    const ProfileResult &validation = *state.sourceValidation;
    if (!validation.ok()) {
      return failure(
          validation.error == ProfileError::None
              ? ProfileError::IntegrityFailure
              : validation.error,
          "profile source is present but cannot be safely retained as a "
          "completed deletion: " +
              validation.message);
    }
    appendDetail(message, "the canonical profile source remains present");
    return failure(ProfileError::IoFailure, std::move(message));
  };
  auto absentWithoutValidTombstone = [&](const DirectoryState &state,
                                         std::string warning) {
    if (!state.tombstoneExists) {
      appendDetail(warning,
                   "the canonical profile is absent and no deletion "
                   "tombstone remains; durability should be rechecked");
    } else {
      const ProfileResult &validation = *state.tombstoneValidation;
      appendDetail(
          warning,
          "the canonical profile is absent, but the deletion tombstone is "
          "invalid and was preserved: " +
              validation.message +
              "; directory durability should be rechecked");
    }
    return success(std::move(profile), std::move(warning));
  };
  auto cleanupCommittedTombstone = [&](std::string warning = {}) {
    std::string cleanupError;
    if (!dependencies.filesystem.removeTree(tombstone, cleanupError)) {
      appendDetail(warning,
                   "profile deletion committed, but tombstone cleanup should "
                   "be retried: " +
                       cleanupError);
      return success(std::move(profile), std::move(warning));
    }
    std::string syncError;
    if (!dependencies.filesystem.syncDirectory(profilesRoot, syncError)) {
      appendDetail(warning,
                   "profile deletion committed, but cleanup directory sync "
                   "should be retried: " +
                       syncError);
    }
    return success(std::move(profile), std::move(warning));
  };

  std::string errorMessage;
  if (!ensureContainedPath(applicationRoot, source.root, errorMessage) ||
      !ensureContainedPath(applicationRoot, tombstone, errorMessage)) {
    return failure(ProfileError::IoFailure,
                   "unable to inspect profile deletion paths: " + errorMessage);
  }

  bool tombstoneExists = false;
  if (!pathExists(tombstone, tombstoneExists, errorMessage)) {
    return failure(ProfileError::IoFailure, errorMessage);
  }
  if (tombstoneExists) {
    if (!dependencies.filesystem.removeTree(tombstone, errorMessage)) {
      return failure(ProfileError::IoFailure,
                     "unable to clean prior deletion tombstone: " +
                         errorMessage);
    }
    if (!dependencies.filesystem.syncDirectory(profilesRoot, errorMessage)) {
      return failure(ProfileError::IoFailure,
                     "unable to sync prior deletion tombstone cleanup: " +
                         errorMessage);
    }
  }

  errorMessage.clear();
  const bool renameReportedSuccess = dependencies.filesystem.durableRename(
      source.root, tombstone, errorMessage);
  const std::string renameError = errorMessage;
  DirectoryState afterRename;
  std::string inspectionError;
  if (!inspectState(afterRename, inspectionError)) {
    return failure(ProfileError::IoFailure,
                   "unable to inspect profile deletion outcome: " +
                       inspectionError);
  }
  if (afterRename.sourceExists) {
    std::string message =
        renameReportedSuccess
            ? "profile deletion staging left the canonical source present"
            : "unable to stage profile deletion: " + renameError;
    return sourceRetainedFailure(afterRename, std::move(message));
  }
  if (!afterRename.tombstoneExists || !afterRename.tombstoneValidation->ok()) {
    std::string warning;
    if (!renameReportedSuccess && !renameError.empty()) {
      warning = "profile rename reported failure: " + renameError;
    }
    return absentWithoutValidTombstone(afterRename, std::move(warning));
  }

  errorMessage.clear();
  if (dependencies.filesystem.syncDirectory(profilesRoot, errorMessage)) {
    return cleanupCommittedTombstone();
  }

  std::string commitFailure =
      "unable to commit profile deletion: " + errorMessage;
  std::string rollbackError;
  const bool rollbackReportedSuccess = dependencies.filesystem.durableRename(
      tombstone, source.root, rollbackError);
  DirectoryState afterRollback;
  inspectionError.clear();
  if (!inspectState(afterRollback, inspectionError)) {
    return failure(ProfileError::IoFailure,
                   commitFailure +
                       "; unable to inspect profile deletion rollback: " +
                       inspectionError);
  }
  if (afterRollback.sourceExists) {
    if (!afterRollback.sourceValidation->ok()) {
      return sourceRetainedFailure(afterRollback, std::move(commitFailure));
    }
    if (!rollbackReportedSuccess && !rollbackError.empty()) {
      appendDetail(commitFailure,
                   "profile rollback reported failure: " + rollbackError);
    }
    std::string rollbackSyncError;
    if (!dependencies.filesystem.syncDirectory(profilesRoot,
                                               rollbackSyncError)) {
      appendDetail(commitFailure, "unable to sync profile deletion rollback: " +
                                      rollbackSyncError);
    }
    return sourceRetainedFailure(afterRollback, std::move(commitFailure));
  }

  if (!afterRollback.tombstoneExists ||
      !afterRollback.tombstoneValidation->ok()) {
    if (!rollbackReportedSuccess && !rollbackError.empty()) {
      appendDetail(commitFailure,
                   "profile rollback reported failure: " + rollbackError);
    }
    std::string retrySyncError;
    if (!dependencies.filesystem.syncDirectory(profilesRoot, retrySyncError)) {
      appendDetail(commitFailure,
                   "profile remains absent, but directory sync should be "
                   "retried: " +
                       retrySyncError);
    }
    return absentWithoutValidTombstone(afterRollback, std::move(commitFailure));
  }

  std::string retrySyncError;
  if (!dependencies.filesystem.syncDirectory(profilesRoot, retrySyncError)) {
    if (!rollbackReportedSuccess && !rollbackError.empty()) {
      appendDetail(commitFailure,
                   "profile rollback reported failure: " + rollbackError);
    }
    appendDetail(commitFailure,
                 "profile remains absent, but directory sync should be "
                 "retried: " +
                     retrySyncError);
    return success(std::move(profile), std::move(commitFailure));
  }
  return cleanupCommittedTombstone();
}

ProfileResult buildProfile(
    const std::filesystem::path &applicationRoot,
    const PlayerProfileManagerDependencies &dependencies, std::string id,
    std::string displayName, BuildMode mode,
    const std::optional<PlayerProfilePaths> &duplicateSource = std::nullopt) {
  const PlayerProfilePaths staging = makeStagingPaths(applicationRoot, id);
  const PlayerProfilePaths destination = makePaths(applicationRoot, id);
  std::string errorMessage;
  auto fail = [&](ProfileError error, std::string message) {
    cleanupStaging(applicationRoot, dependencies, staging.root, message);
    return failure(error, std::move(message));
  };
  auto migrationPhase = [&](ProfileMigrationPhase phase) {
    return mode != BuildMode::Migration ||
           runPhase(dependencies, phase, errorMessage);
  };

  if (!ensureSafeProfilesRoot(applicationRoot, true, errorMessage) ||
      !ensureContainedPath(applicationRoot, staging.root, errorMessage) ||
      !ensureContainedPath(applicationRoot, destination.root, errorMessage)) {
    return fail(ProfileError::IoFailure, errorMessage);
  }

  if (mode == BuildMode::Migration) {
    for (const auto &[source, supportedVersion] :
         std::array<std::pair<std::filesystem::path, int>, 2>{
             std::pair{applicationRoot / "db" / "score.db",
                       ScoreDBHelper::kCurrentSchemaVersion},
             std::pair{applicationRoot / "db" / "replay.db",
                       ReplayDBHelper::kCurrentSchemaVersion}}) {
      bool exists = false;
      if (!pathExists(source, exists, errorMessage)) {
        return fail(ProfileError::MigrationFailure, errorMessage);
      }
      if (!exists) {
        continue;
      }
      std::error_code statusError;
      const auto status = std::filesystem::symlink_status(source, statusError);
      if (statusError || hasUnsafeLink(source, status, errorMessage) ||
          !std::filesystem::is_regular_file(status)) {
        if (errorMessage.empty()) {
          errorMessage = "legacy profile database is not a safe regular file";
        }
        return fail(ProfileError::MigrationFailure, errorMessage);
      }
      const auto version = sqliteDatabaseUserVersion(source, errorMessage);
      if (!version) {
        return fail(ProfileError::MigrationFailure,
                    "unable to inspect legacy database version: " +
                        errorMessage);
      }
      if (*version > supportedVersion) {
        return fail(ProfileError::FutureVersion,
                    "legacy profile database is newer than supported");
      }
    }
  }

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
  if (!cleanupStaging(applicationRoot, dependencies, staging.root,
                      errorMessage)) {
    return fail(ProfileError::IoFailure, errorMessage);
  }
  std::error_code filesystemError;
  const bool stagingCreated =
      std::filesystem::create_directory(staging.root, filesystemError);
  if (filesystemError || !stagingCreated) {
    return fail(ProfileError::IoFailure,
                "unable to create profile staging directory: " +
                    (filesystemError ? filesystemError.message()
                                     : "path already exists"));
  }

  if (mode == BuildMode::Duplicate) {
    if (!copyPracticeDirectory(applicationRoot, *duplicateSource, staging,
                               errorMessage)) {
      return fail(ProfileError::IoFailure,
                  "unable to duplicate practice data: " + errorMessage);
    }
  } else {
    filesystemError.clear();
    if (!std::filesystem::create_directory(staging.practiceDirectory,
                                           filesystemError) ||
        filesystemError) {
      return fail(ProfileError::IoFailure,
                  "unable to create profile practice directory: " +
                      filesystemError.message());
    }
  }

  if (!migrationPhase(ProfileMigrationPhase::WriteSettings)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  if (mode == BuildMode::Duplicate) {
    const auto settings = AppSettingsStore::Load(duplicateSource->settingsJson);
    if (settings.status != AppSettingsLoadStatus::Loaded) {
      return fail(ProfileError::IoFailure,
                  "unable to load duplicate profile settings");
    }
    if (!AppSettingsStore::Save(staging.settingsJson, settings.settings,
                                errorMessage)) {
      return fail(ProfileError::IoFailure,
                  "unable to duplicate profile settings: " + errorMessage);
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
    const auto input = InputProfileStore::load(duplicateSource->inputJson);
    if (input.status != InputProfileLoadStatus::Loaded) {
      return fail(ProfileError::IoFailure,
                  "unable to load duplicate input profile");
    }
    if (!InputProfileStore::saveAtomic(staging.inputJson, input.profile,
                                       errorMessage)) {
      return fail(ProfileError::IoFailure,
                  "unable to duplicate input profile: " + errorMessage);
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

  for (const auto &file :
       {staging.settingsJson, staging.inputJson, staging.scoresDb,
        staging.replaysDb, staging.profileJson}) {
    if (!ensureContainedPath(applicationRoot, file, errorMessage) ||
        !dependencies.filesystem.syncFile(file, errorMessage)) {
      return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                               : ProfileError::IoFailure,
                  "unable to make staged profile durable: " + errorMessage);
    }
  }
  std::vector<std::filesystem::path> practiceFiles;
  if (!validatePracticeDirectory(applicationRoot, staging, &practiceFiles,
                                 errorMessage)) {
    return fail(ProfileError::IntegrityFailure, errorMessage);
  }
  for (const auto &file : practiceFiles) {
    if (!dependencies.filesystem.syncFile(file, errorMessage)) {
      return fail(ProfileError::IoFailure,
                  "unable to make staged practice data durable: " +
                      errorMessage);
    }
  }
  if (!dependencies.filesystem.syncDirectory(staging.practiceDirectory,
                                             errorMessage)) {
    return fail(ProfileError::IoFailure,
                "unable to sync staged practice directory: " + errorMessage);
  }
  if (!dependencies.filesystem.syncDirectory(staging.root, errorMessage)) {
    return fail(mode == BuildMode::Migration ? ProfileError::MigrationFailure
                                             : ProfileError::IoFailure,
                "unable to sync staged profile directory: " + errorMessage);
  }

  if (!migrationPhase(ProfileMigrationPhase::FinalizeProfile)) {
    return fail(ProfileError::MigrationFailure, errorMessage);
  }
  if (mode == BuildMode::Migration) {
    if (!ensureContainedPath(applicationRoot, staging.root, errorMessage) ||
        !ensureContainedPath(applicationRoot, destination.root, errorMessage) ||
        !dependencies.filesystem.durableRename(staging.root, destination.root,
                                               errorMessage)) {
      return fail(ProfileError::MigrationFailure,
                  "unable to finalize profile: " + errorMessage);
    }
    if (!dependencies.filesystem.syncDirectory(applicationRoot / "profiles",
                                               errorMessage)) {
      return fail(ProfileError::MigrationFailure,
                  "unable to sync finalized profile directory: " +
                      errorMessage);
    }
    return success(std::move(profile));
  }
  return finalizeNewProfileDirectory(applicationRoot, dependencies, staging,
                                     destination, std::move(profile));
}

bool cleanupAbandonedStaging(
    const std::filesystem::path &applicationRoot,
    const PlayerProfileManagerDependencies &dependencies,
    std::string &errorMessage) {
  const auto profiles = applicationRoot / "profiles";
  if (!ensureSafeProfilesRoot(applicationRoot, true, errorMessage)) {
    return false;
  }
  std::error_code error;
  std::filesystem::directory_iterator iterator(profiles, error);
  if (error) {
    errorMessage = "unable to enumerate profiles directory: " + error.message();
    return false;
  }
  for (const auto &entry : iterator) {
    const std::string filename = entry.path().filename().string();
    if (filename.starts_with(".backup-")) {
      const std::string id = filename.substr(std::string(".backup-").size());
      if (!isUuid(id)) {
        errorMessage = "invalid abandoned profile backup name";
        return false;
      }
      const PlayerProfilePaths destination = makePaths(applicationRoot, id);
      const PlayerProfilePaths backup = makePathsAtRoot(entry.path());
      bool destinationExists = false;
      if (!ensureContainedPath(applicationRoot, entry.path(), errorMessage) ||
          !ensureContainedPath(applicationRoot, destination.root,
                               errorMessage) ||
          !pathExists(destination.root, destinationExists, errorMessage)) {
        return false;
      }
      if (destinationExists) {
        const ProfileResult destinationValidation =
            validateProfileFiles(applicationRoot, destination, id, true);
        if (destinationValidation.ok()) {
          if (!dependencies.filesystem.removeTree(entry.path(), errorMessage)) {
            errorMessage =
                "unable to remove completed profile backup: " + errorMessage;
            return false;
          }
        } else {
          if (destinationValidation.error != ProfileError::IntegrityFailure) {
            errorMessage =
                "profile overwrite recovery cannot classify the destination "
                "as corrupt (" +
                destinationValidation.message +
                "); retaining destination and "
                "backup";
            return false;
          }
          const ProfileResult backupValidation =
              validateProfileFiles(applicationRoot, backup, id, true);
          if (!backupValidation.ok()) {
            errorMessage =
                "profile overwrite recovery found an invalid destination (" +
                destinationValidation.message + ") and invalid backup (" +
                backupValidation.message + "); retaining both";
            return false;
          }
          if (!dependencies.filesystem.removeTree(destination.root,
                                                  errorMessage)) {
            errorMessage =
                "unable to remove partial profile replacement: " + errorMessage;
            return false;
          }
          if (!dependencies.filesystem.syncDirectory(profiles, errorMessage)) {
            errorMessage =
                "unable to sync partial profile removal: " + errorMessage;
            return false;
          }
          if (!dependencies.filesystem.durableRename(
                  backup.root, destination.root, errorMessage)) {
            errorMessage =
                "unable to restore valid profile backup: " + errorMessage;
            return false;
          }
        }
      } else {
        const ProfileResult backupValidation =
            validateProfileFiles(applicationRoot, backup, id, true);
        if (!backupValidation.ok()) {
          errorMessage = "abandoned profile backup is invalid: " +
                         backupValidation.message;
          return false;
        }
        if (!dependencies.filesystem.durableRename(
                backup.root, destination.root, errorMessage)) {
          errorMessage =
              "unable to restore abandoned profile backup: " + errorMessage;
          return false;
        }
      }
      if (!dependencies.filesystem.syncDirectory(profiles, errorMessage)) {
        errorMessage =
            "unable to sync profile backup recovery: " + errorMessage;
        return false;
      }
      continue;
    }
    if (!filename.starts_with(".staging-") &&
        !filename.starts_with(".deleting-")) {
      continue;
    }
    if (!ensureContainedPath(applicationRoot, entry.path(), errorMessage) ||
        !dependencies.filesystem.removeTree(entry.path(), errorMessage)) {
      errorMessage =
          "unable to remove abandoned profile transaction: " + errorMessage;
      return false;
    }
    if (!dependencies.filesystem.syncDirectory(profiles, errorMessage)) {
      errorMessage =
          "unable to sync abandoned transaction cleanup: " + errorMessage;
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
  if (!dependencies_.filesystem.syncFile) {
    dependencies_.filesystem.syncFile = atomic_file::syncFile;
  }
  if (!dependencies_.filesystem.syncDirectory) {
    dependencies_.filesystem.syncDirectory = atomic_file::syncDirectory;
  }
  if (!dependencies_.filesystem.durableRename) {
    dependencies_.filesystem.durableRename = atomic_file::renameDurably;
  }
  if (!dependencies_.filesystem.removeTree) {
    dependencies_.filesystem.removeTree = removeTree;
  }
}

ProfileResult PlayerProfileManager::Initialize() {
  activeProfile_.reset();
  std::string errorMessage;
  if (!cleanupAbandonedStaging(applicationDataRoot_, dependencies_,
                               errorMessage)) {
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
    const ProfileResult validated =
        validateProfile(bootstrap.id, ValidationDepth::Routine,
                        DatabaseVersionPolicy::AllowSupportedOlder);
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
    const ProfileResult validated = validateProfile(
        backup.id, ValidationDepth::Deep,
        DatabaseVersionPolicy::AllowSupportedOlder);
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

  std::vector<PlayerProfile> profiles = listProfiles(
      ValidationDepth::Deep, DatabaseVersionPolicy::AllowSupportedOlder);
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
    const ProfileResult candidate = validateProfile(
        candidateId, ValidationDepth::Deep,
        DatabaseVersionPolicy::AllowSupportedOlder);
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

  ProfileResult built = buildProfile(applicationDataRoot_, dependencies_, id,
                                     "Player", BuildMode::Migration);
  if (!built.ok()) {
    return built;
  }
  if (!runPhase(dependencies_, ProfileMigrationPhase::WriteBootstrap,
                errorMessage)) {
    return failure(ProfileError::MigrationFailure, errorMessage);
  }
  if (!writeBootstrap(applicationDataRoot_, id, errorMessage)) {
    return failure(ProfileError::MigrationFailure,
                   "unable to write active profile bootstrap: " + errorMessage);
  }
  activeProfile_ = built.profile;
  return built;
}

const PlayerProfile &PlayerProfileManager::activeProfile() const {
  if (!activeProfile_) {
    throw std::logic_error("PlayerProfileManager is not initialized");
  }
  return *activeProfile_;
}

const std::filesystem::path &PlayerProfileManager::applicationDataRoot() const {
  return applicationDataRoot_;
}

PlayerProfilePaths PlayerProfileManager::activePaths() const {
  return activeProfile_ ? pathsFor(activeProfile_->id) : PlayerProfilePaths{};
}

PlayerProfilePaths PlayerProfileManager::pathsFor(std::string_view id) const {
  return makePaths(applicationDataRoot_, id);
}

std::vector<PlayerProfile> PlayerProfileManager::listProfiles() const {
  return listProfiles(ValidationDepth::Routine,
                      DatabaseVersionPolicy::AllowSupportedOlder);
}

std::vector<PlayerProfile>
PlayerProfileManager::listProfiles(ValidationDepth depth,
                                   DatabaseVersionPolicy policy) const {
  std::vector<PlayerProfile> profiles;
  std::string safetyError;
  if (!ensureSafeProfilesRoot(applicationDataRoot_, false, safetyError)) {
    return profiles;
  }
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
    const ProfileResult validated = validateProfile(id, depth, policy);
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
  std::string rootError;
  if (!ensureSafeProfilesRoot(applicationDataRoot_, false, rootError)) {
    return failure(ProfileError::IoFailure, rootError);
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
                      BuildMode::Create);
}

ProfileResult PlayerProfileManager::duplicateProfile(std::string_view sourceId,
                                                     std::string displayName) {
  if (!activeProfile_) {
    return failure(ProfileError::SwitchBlocked,
                   "profile manager is not initialized");
  }
  std::string rootError;
  if (!ensureSafeProfilesRoot(applicationDataRoot_, false, rootError)) {
    return failure(ProfileError::IoFailure, rootError);
  }
  const ProfileResult source = validateProfile(
      sourceId, ValidationDepth::Deep,
      DatabaseVersionPolicy::AllowSupportedOlder);
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
                      BuildMode::Duplicate, pathsFor(sourceId));
}

ProfileResult PlayerProfileManager::renameProfile(std::string_view id,
                                                  std::string displayName) {
  std::string rootError;
  if (!ensureSafeProfilesRoot(applicationDataRoot_, false, rootError)) {
    return failure(ProfileError::IoFailure, rootError);
  }
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
  std::string rootError;
  if (!ensureSafeProfilesRoot(applicationDataRoot_, false, rootError)) {
    return failure(ProfileError::IoFailure, rootError);
  }
  const ProfileResult validated = validateProfile(id);
  if (!validated.ok()) {
    return validated;
  }
  const auto profiles =
      listProfiles(ValidationDepth::Deep, DatabaseVersionPolicy::CurrentOnly);
  if (profiles.size() <= 1) {
    return failure(ProfileError::LastProfileDeletion,
                   "the last profile cannot be deleted");
  }
  if (activeProfile_->id == id) {
    return failure(ProfileError::ActiveProfileDeletion,
                   "the active profile cannot be deleted");
  }

  const PlayerProfilePaths source = pathsFor(id);
  const auto tombstone =
      applicationDataRoot_ / "profiles" / (".deleting-" + std::string(id));
  return finalizeProfileDeletion(applicationDataRoot_, dependencies_, source,
                                 tombstone, *validated.profile);
}

ProfileResult PlayerProfileManager::validateProfile(std::string_view id) const {
  return validateProfile(id, ValidationDepth::Deep,
                         DatabaseVersionPolicy::CurrentOnly);
}

ProfileResult
PlayerProfileManager::validateProfileForActivation(std::string_view id) const {
  return validateProfile(id, ValidationDepth::Deep,
                         DatabaseVersionPolicy::AllowSupportedOlder);
}

ProfileResult
PlayerProfileManager::validateProfile(std::string_view id,
                                      ValidationDepth depth,
                                      DatabaseVersionPolicy policy) const {
  if (!isUuid(id)) {
    return failure(ProfileError::NotFound, "profile UUID is invalid");
  }
  const PlayerProfilePaths paths = pathsFor(id);
  std::string safetyError;
  if (!ensureSafeProfilesRoot(applicationDataRoot_, false, safetyError) ||
      !ensureContainedPath(applicationDataRoot_, paths.root, safetyError) ||
      !ensureContainedPath(applicationDataRoot_, paths.profileJson,
                           safetyError)) {
    return failure(ProfileError::IoFailure, safetyError);
  }
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
    if (!ensureContainedPath(applicationDataRoot_, file, safetyError)) {
      return failure(ProfileError::IntegrityFailure, safetyError);
    }
    error.clear();
    const auto status = std::filesystem::symlink_status(file, error);
    if (error || !std::filesystem::is_regular_file(status)) {
      return failure(ProfileError::IntegrityFailure,
                     "profile component is missing: " +
                         file.filename().string());
    }
  }

  if (!validatePracticeDirectory(applicationDataRoot_, paths, nullptr,
                                 safetyError)) {
    return failure(ProfileError::IntegrityFailure, safetyError);
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
  if (policy == DatabaseVersionPolicy::CurrentOnly &&
      (*scoreVersion != ScoreDBHelper::kCurrentSchemaVersion ||
       *replayVersion != ReplayDBHelper::kCurrentSchemaVersion)) {
    return failure(ProfileError::IntegrityFailure,
                   "profile database schema is not current");
  }
  if (depth == ValidationDepth::Deep &&
      (!sqliteIntegrityCheck(paths.scoresDb, errorMessage) ||
       !sqliteIntegrityCheck(paths.replaysDb, errorMessage))) {
    return failure(ProfileError::IntegrityFailure, errorMessage);
  }
  return metadata;
}

ProfileResult PlayerProfileManager::commitActiveProfile(std::string_view id) {
  if (!activeProfile_) {
    return failure(ProfileError::SwitchBlocked,
                   "profile manager is not initialized");
  }
  std::string rootError;
  if (!ensureSafeProfilesRoot(applicationDataRoot_, false, rootError)) {
    return failure(ProfileError::IoFailure, rootError);
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
  if (!runPhase(dependencies_, ProfileMigrationPhase::WriteBootstrap,
                errorMessage) ||
      !writeBootstrap(applicationDataRoot_, id, errorMessage)) {
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

ProfileResult PlayerProfileManager::installProfile(
    PlayerProfile sourceProfile, std::optional<std::string> overwriteProfileId,
    ProfileStagingWriter writeStaging) {
  if (!activeProfile_) {
    return failure(ProfileError::SwitchBlocked,
                   "profile manager is not initialized");
  }
  if (!writeStaging) {
    return failure(ProfileError::IoFailure,
                   "profile staging writer is unavailable");
  }
  std::string errorMessage;
  if (!ensureSafeProfilesRoot(applicationDataRoot_, false, errorMessage)) {
    return failure(ProfileError::IoFailure, errorMessage);
  }
  const auto name = normalizedName(sourceProfile.displayName);
  if (!name || *name != sourceProfile.displayName) {
    return failure(ProfileError::InvalidName,
                   "imported profile name is invalid");
  }
  if (sourceProfile.createdAt.empty()) {
    return failure(ProfileError::IntegrityFailure,
                   "imported profile creation timestamp is missing");
  }

  const bool overwrite = overwriteProfileId.has_value();
  std::string id;
  if (overwrite) {
    const ProfileResult target = validateProfile(
        *overwriteProfileId, ValidationDepth::Deep,
        DatabaseVersionPolicy::AllowSupportedOlder);
    if (!target.ok()) {
      return target;
    }
    if (listProfiles(ValidationDepth::Deep,
                     DatabaseVersionPolicy::AllowSupportedOlder)
            .size() <= 1) {
      return failure(ProfileError::LastProfileDeletion,
                     "the last profile cannot be overwritten");
    }
    if (activeProfile_->id == *overwriteProfileId) {
      return failure(ProfileError::ActiveProfileDeletion,
                     "the active profile cannot be overwritten");
    }
    id = *overwriteProfileId;
  } else {
    for (int attempt = 0; attempt < 64; ++attempt) {
      const std::string candidate = dependencies_.generateUuid();
      if (!isUuid(candidate)) {
        continue;
      }
      bool destinationExists = false;
      bool stagingExists = false;
      bool backupExists = false;
      if (!pathExists(makePaths(applicationDataRoot_, candidate).root,
                      destinationExists, errorMessage) ||
          !pathExists(makeStagingPaths(applicationDataRoot_, candidate).root,
                      stagingExists, errorMessage) ||
          !pathExists(makeBackupPath(applicationDataRoot_, candidate),
                      backupExists, errorMessage)) {
        return failure(ProfileError::IoFailure, errorMessage);
      }
      if (!destinationExists && !stagingExists && !backupExists) {
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
  }

  sourceProfile.schemaVersion = kPlayerProfileSchemaVersion;
  sourceProfile.id = id;
  sourceProfile.displayName = *name;
  sourceProfile.lastUsedAt = dependencies_.utcNow();
  const PlayerProfilePaths staging = makeStagingPaths(applicationDataRoot_, id);
  const PlayerProfilePaths destination = makePaths(applicationDataRoot_, id);
  const auto backup = makeBackupPath(applicationDataRoot_, id);

  auto cleanStagingAndFail = [&](ProfileError error, std::string message) {
    cleanupStaging(applicationDataRoot_, dependencies_, staging.root, message);
    return failure(error, std::move(message));
  };
  if (!ensureContainedPath(applicationDataRoot_, staging.root, errorMessage) ||
      !ensureContainedPath(applicationDataRoot_, destination.root,
                           errorMessage) ||
      !ensureContainedPath(applicationDataRoot_, backup, errorMessage)) {
    return cleanStagingAndFail(ProfileError::IoFailure, errorMessage);
  }

  bool stagingExists = false;
  bool backupExists = false;
  bool destinationExists = false;
  if (!pathExists(staging.root, stagingExists, errorMessage) ||
      !pathExists(backup, backupExists, errorMessage) ||
      !pathExists(destination.root, destinationExists, errorMessage)) {
    return cleanStagingAndFail(ProfileError::IoFailure, errorMessage);
  }
  if (backupExists) {
    return cleanStagingAndFail(ProfileError::IoFailure,
                               "an unfinished profile backup exists");
  }
  if ((overwrite && !destinationExists) || (!overwrite && destinationExists)) {
    return cleanStagingAndFail(ProfileError::IoFailure,
                               overwrite ? "overwrite target disappeared"
                                         : "profile UUID already exists");
  }
  if (stagingExists && !cleanupStaging(applicationDataRoot_, dependencies_,
                                       staging.root, errorMessage)) {
    return failure(ProfileError::IoFailure, errorMessage);
  }
  std::error_code filesystemError;
  const bool stagingCreated =
      std::filesystem::create_directory(staging.root, filesystemError);
  if (filesystemError || !stagingCreated) {
    return cleanStagingAndFail(ProfileError::IoFailure,
                               "unable to create profile staging directory: " +
                                   (filesystemError ? filesystemError.message()
                                                    : "path already exists"));
  }

  if (!writeStaging(staging, errorMessage)) {
    return cleanStagingAndFail(ProfileError::IoFailure,
                               "unable to stage imported profile: " +
                                   errorMessage);
  }
  bool practiceDirectoryExists = false;
  if (!pathExists(staging.practiceDirectory, practiceDirectoryExists,
                  errorMessage)) {
    return cleanStagingAndFail(ProfileError::IoFailure, errorMessage);
  }
  if (!practiceDirectoryExists) {
    filesystemError.clear();
    if (!std::filesystem::create_directory(staging.practiceDirectory,
                                           filesystemError) ||
        filesystemError) {
      return cleanStagingAndFail(
          ProfileError::IoFailure,
          "unable to create imported practice directory: " +
              filesystemError.message());
    }
  }
  if (!writeProfileMetadata(staging.profileJson, sourceProfile, errorMessage)) {
    return cleanStagingAndFail(ProfileError::IoFailure,
                               "unable to write imported profile metadata: " +
                                   errorMessage);
  }
  const auto scoreRowsBefore =
      sqliteUserTableRowCounts(staging.scoresDb, errorMessage);
  const auto replayRowsBefore =
      sqliteUserTableRowCounts(staging.replaysDb, errorMessage);
  const auto scoreVersionBefore =
      sqliteDatabaseUserVersion(staging.scoresDb, errorMessage);
  const auto replayVersionBefore =
      sqliteDatabaseUserVersion(staging.replaysDb, errorMessage);
  if (!scoreRowsBefore || !replayRowsBefore || !scoreVersionBefore ||
      !replayVersionBefore) {
    return cleanStagingAndFail(ProfileError::IntegrityFailure,
                               "unable to inspect imported databases: " +
                                   errorMessage);
  }
  if (*scoreVersionBefore > ScoreDBHelper::kCurrentSchemaVersion ||
      *replayVersionBefore > ReplayDBHelper::kCurrentSchemaVersion) {
    return cleanStagingAndFail(ProfileError::FutureVersion,
                               "imported database is newer than supported");
  }
  if (!app_database_initializer::initializeScoreDatabase(staging.scoresDb) ||
      !app_database_initializer::initializeReplayDatabase(staging.replaysDb)) {
    return cleanStagingAndFail(ProfileError::IntegrityFailure,
                               "unable to migrate imported databases");
  }
  const auto scoreRowsAfter =
      sqliteUserTableRowCounts(staging.scoresDb, errorMessage);
  const auto replayRowsAfter =
      sqliteUserTableRowCounts(staging.replaysDb, errorMessage);
  auto rowsPreserved = [](const auto &before, const auto &after) {
    if (!before || !after) {
      return false;
    }
    for (const auto &[table, count] : *before) {
      const auto found = after->find(table);
      if (found == after->end() || found->second != count) {
        return false;
      }
    }
    return true;
  };
  if (!rowsPreserved(scoreRowsBefore, scoreRowsAfter) ||
      !rowsPreserved(replayRowsBefore, replayRowsAfter)) {
    return cleanStagingAndFail(
        ProfileError::IntegrityFailure,
        "database migration changed imported row counts");
  }
  ProfileResult staged = validateProfileFiles(
      applicationDataRoot_, staging, sourceProfile.id, false);
  if (!staged.ok()) {
    return cleanStagingAndFail(
        staged.error, "staged imported profile is invalid: " + staged.message);
  }
  for (const auto &file :
       {staging.settingsJson, staging.inputJson, staging.scoresDb,
        staging.replaysDb, staging.profileJson}) {
    if (!dependencies_.filesystem.syncFile(file, errorMessage)) {
      return cleanStagingAndFail(ProfileError::IoFailure,
                                 "unable to make imported profile durable: " +
                                     errorMessage);
    }
  }
  std::vector<std::filesystem::path> importedPracticeFiles;
  if (!validatePracticeDirectory(applicationDataRoot_, staging,
                                 &importedPracticeFiles, errorMessage)) {
    return cleanStagingAndFail(ProfileError::IntegrityFailure, errorMessage);
  }
  for (const auto &file : importedPracticeFiles) {
    if (!dependencies_.filesystem.syncFile(file, errorMessage)) {
      return cleanStagingAndFail(ProfileError::IoFailure,
                                 "unable to make imported practice data "
                                 "durable: " +
                                     errorMessage);
    }
  }
  if (!dependencies_.filesystem.syncDirectory(staging.practiceDirectory,
                                              errorMessage)) {
    return cleanStagingAndFail(ProfileError::IoFailure,
                               "unable to sync imported practice directory: " +
                                   errorMessage);
  }
  if (!dependencies_.filesystem.syncDirectory(staging.root, errorMessage)) {
    return cleanStagingAndFail(ProfileError::IoFailure,
                               "unable to sync imported profile staging: " +
                                   errorMessage);
  }

  const auto profilesRoot = applicationDataRoot_ / "profiles";
  if (!overwrite) {
    return finalizeNewProfileDirectory(
        applicationDataRoot_, dependencies_, staging, destination,
        std::move(sourceProfile));
  }

  auto rollback = [&](std::string message) {
    std::string rollbackError;
    bool oldExists = false;
    if (!pathExists(backup, oldExists, rollbackError)) {
      message += "; unable to inspect rollback backup: " + rollbackError;
    } else if (!oldExists) {
      message += "; rollback backup is missing; retaining replacement";
    } else {
      const ProfileResult backupValidation = validateProfileFiles(
          applicationDataRoot_, makePathsAtRoot(backup), id, true);
      if (!backupValidation.ok()) {
        message += "; rollback backup is not safely recoverable (" +
                   backupValidation.message +
                   "); retaining replacement and "
                   "backup";
      } else {
        bool currentExists = false;
        rollbackError.clear();
        if (!pathExists(destination.root, currentExists, rollbackError)) {
          message += "; unable to inspect failed replacement: " + rollbackError;
        } else if (currentExists && !dependencies_.filesystem.removeTree(
                                        destination.root, rollbackError)) {
          message += "; unable to remove failed replacement: " + rollbackError +
                     "; retaining valid backup";
        } else {
          rollbackError.clear();
          if (!dependencies_.filesystem.durableRename(backup, destination.root,
                                                      rollbackError)) {
            message += "; unable to restore original profile: " + rollbackError;
          }
        }
      }
    }
    rollbackError.clear();
    if (!dependencies_.filesystem.syncDirectory(profilesRoot, rollbackError)) {
      message += "; unable to sync profile rollback: " + rollbackError;
    }
    cleanupStaging(applicationDataRoot_, dependencies_, staging.root, message);
    return failure(ProfileError::IoFailure, std::move(message));
  };

  if (!dependencies_.filesystem.durableRename(destination.root, backup,
                                              errorMessage)) {
    return cleanStagingAndFail(ProfileError::IoFailure,
                               "unable to back up overwrite target: " +
                                   errorMessage);
  }
  if (!dependencies_.filesystem.syncDirectory(profilesRoot, errorMessage)) {
    return rollback("unable to sync overwrite backup: " + errorMessage);
  }
  if (!dependencies_.filesystem.durableRename(staging.root, destination.root,
                                              errorMessage)) {
    return rollback("unable to replace profile: " + errorMessage);
  }
  if (!dependencies_.filesystem.syncDirectory(profilesRoot, errorMessage)) {
    return rollback("unable to sync replaced profile: " + errorMessage);
  }
  // The replacement and its parent-directory entry are durable at this point.
  // Backup cleanup is post-commit: a partial cleanup must never roll a valid
  // replacement back onto a potentially damaged old tree. Initialize() will
  // remove any surviving .backup-* tree while retaining the destination.
  std::string cleanupWarning;
  if (!dependencies_.filesystem.removeTree(backup, errorMessage)) {
    cleanupWarning =
        "profile overwrite committed, but backup cleanup is deferred: " +
        errorMessage;
  }
  std::string finalSyncError;
  if (!dependencies_.filesystem.syncDirectory(profilesRoot, finalSyncError)) {
    if (!cleanupWarning.empty()) {
      cleanupWarning += "; ";
    }
    cleanupWarning +=
        "profile overwrite committed, but cleanup sync should be retried: " +
        finalSyncError;
  }
  return success(std::move(sourceProfile), std::move(cleanupWarning));
}
