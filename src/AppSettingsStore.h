#pragma once

#include "AppSettings.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

enum class AppSettingsLoadStatus { Loaded, Missing, Invalid, FutureVersion };

struct AppSettingsLoadResult {
  AppSettings settings;
  AppSettingsLoadStatus status = AppSettingsLoadStatus::Missing;
  std::vector<std::string> diagnostics;
};

class AppSettingsStore {
public:
  static constexpr int kCurrentSchemaVersion = 7;

  static AppSettingsLoadResult Load(const std::filesystem::path &settingsJson);
  static AppSettingsLoadResult
  LoadLegacyCfg(const std::filesystem::path &settingsCfg);
  static bool Save(const std::filesystem::path &settingsJson,
                   const AppSettings &settings, std::string &errorMessage);
#ifdef APP_SETTINGS_STORE_TESTING
  static AppSettingsLoadResult
  LoadLegacyCfgStreamForTesting(std::istream &input);
#endif
};
