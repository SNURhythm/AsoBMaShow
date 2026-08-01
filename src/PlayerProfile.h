#pragma once

#include <filesystem>
#include <string>

inline constexpr int kPlayerProfileSchemaVersion = 1;
inline constexpr int kActiveProfileSchemaVersion = 1;

struct PlayerProfile {
  int schemaVersion = kPlayerProfileSchemaVersion;
  std::string id;
  std::string displayName;
  std::string createdAt;
  std::string lastUsedAt;

  bool operator==(const PlayerProfile &) const = default;
};

struct PlayerProfilePaths {
  std::filesystem::path root;
  std::filesystem::path profileJson;
  std::filesystem::path settingsJson;
  std::filesystem::path inputJson;
  std::filesystem::path irCredentialsJson;
  std::filesystem::path bokutachiCacheJson;
  std::filesystem::path scoresDb;
  std::filesystem::path replaysDb;
  std::filesystem::path practiceDirectory;
  std::filesystem::path replayDirectory;

  bool operator==(const PlayerProfilePaths &) const = default;
};
