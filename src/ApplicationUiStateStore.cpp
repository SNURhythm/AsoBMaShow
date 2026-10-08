#include "ApplicationUiStateStore.h"

#include "VersionedJson.h"
#include "i18n/Localization.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace {
using nlohmann::json;

std::string_view modeName(MusicSelectToolbarMode mode) {
  switch (mode) {
  case MusicSelectToolbarMode::Expanded:
    return "expanded";
  case MusicSelectToolbarMode::Collapsed:
    return "collapsed";
  case MusicSelectToolbarMode::Hidden:
    return "hidden";
  }
  return "expanded";
}

bool decodeMode(const json &encoded, MusicSelectToolbarMode &mode) {
  if (!encoded.is_string()) {
    return false;
  }
  const auto value = encoded.get<std::string>();
  if (value == "expanded") {
    mode = MusicSelectToolbarMode::Expanded;
    return true;
  }
  if (value == "collapsed") {
    mode = MusicSelectToolbarMode::Collapsed;
    return true;
  }
  if (value == "hidden") {
    mode = MusicSelectToolbarMode::Hidden;
    return true;
  }
  return false;
}

ApplicationUiStateLoadStatus
mapStatus(versioned_json::LoadStatus status) {
  switch (status) {
  case versioned_json::LoadStatus::Loaded:
    return ApplicationUiStateLoadStatus::Loaded;
  case versioned_json::LoadStatus::Missing:
    return ApplicationUiStateLoadStatus::Missing;
  case versioned_json::LoadStatus::FutureVersion:
    return ApplicationUiStateLoadStatus::FutureVersion;
  case versioned_json::LoadStatus::IoError:
  case versioned_json::LoadStatus::Malformed:
  case versioned_json::LoadStatus::InvalidRoot:
  case versioned_json::LoadStatus::MigrationFailed:
    return ApplicationUiStateLoadStatus::Invalid;
  }
  return ApplicationUiStateLoadStatus::Invalid;
}
} // namespace

std::filesystem::path
applicationUiStatePath(const std::filesystem::path &applicationDataRoot) {
  return applicationDataRoot / "application-ui-state.json";
}

ApplicationUiStateLoadResult
ApplicationUiStateStore::Load(const std::filesystem::path &path) {
  static const std::array<versioned_json::Migration, 1> migrations = {
      [](json &, std::string &) { return true; }};
  auto loaded = versioned_json::loadAndMigrate(
      path, ApplicationUiState::kSchemaVersion, migrations);
  ApplicationUiStateLoadResult result{
      .status = mapStatus(loaded.status),
      .diagnostics = std::move(loaded.diagnostics),
  };
  if (loaded.status != versioned_json::LoadStatus::Loaded) {
    return result;
  }

  const auto language = loaded.document.find("language");
  if (language != loaded.document.end()) {
    if (language->is_string() &&
        i18n::isLanguagePreference(language->get<std::string>())) {
      result.state.language = language->get<std::string>();
    } else {
      result.diagnostics.emplace_back("Unsupported language; using system language");
    }
  }

  const auto defaults = loaded.document.find("defaultDifficultyTablesSeeded");
  if (defaults != loaded.document.end() && defaults->is_boolean()) {
    result.state.onlineDifficultyTablesRevision = defaults->get<bool>() ? 1 : 0;
  }
  // Revision 1 shipped the original defaults; revision 2 added the Aery pair.
  // Read old flags only for migration, and preserve revisions from newer builds.
  result.state.bundledDifficultyTablesRevision =
      result.state.onlineDifficultyTablesRevision;
  const auto aery = loaded.document.find("aeryDifficultyTablesSeeded");
  if (aery != loaded.document.end() && aery->is_boolean() && aery->get<bool>()) {
    result.state.bundledDifficultyTablesRevision = 2;
  }
  for (const auto &[key, value] : {
           std::pair{"bundledDifficultyTablesRevision",
                     &result.state.bundledDifficultyTablesRevision},
           std::pair{"onlineDifficultyTablesRevision",
                     &result.state.onlineDifficultyTablesRevision}}) {
    const auto revision = loaded.document.find(key);
    if (revision != loaded.document.end() && revision->is_number_integer() &&
        *revision >= 0 && *revision <= std::numeric_limits<int>::max()) {
      *value = std::max(*value, revision->get<int>());
    }
  }

  const auto toolbar = loaded.document.find("musicSelectToolbar");
  const auto tutorial = loaded.document.find("newcomerTutorialCompleted");
  if (tutorial != loaded.document.end()) {
    if (tutorial->is_boolean()) {
      result.state.newcomerTutorialCompleted = tutorial->get<bool>();
    } else {
      result.diagnostics.emplace_back("Invalid tutorial completion; offering tutorial");
    }
  }
  if (toolbar == loaded.document.end() || !toolbar->is_object()) {
    result.status = ApplicationUiStateLoadStatus::Invalid;
    result.diagnostics.emplace_back(
        "musicSelectToolbar must be an object");
    return result;
  }
  const auto mode = toolbar->find("mode");
  const auto x = toolbar->find("x");
  const auto y = toolbar->find("y");
  const auto hasPosition = toolbar->find("hasPosition");
  if (mode == toolbar->end() ||
      !decodeMode(*mode, result.state.musicSelectToolbar.mode) ||
      x == toolbar->end() || !x->is_number() || y == toolbar->end() ||
      !y->is_number() || hasPosition == toolbar->end() ||
      !hasPosition->is_boolean()) {
    result.status = ApplicationUiStateLoadStatus::Invalid;
    result.diagnostics.emplace_back(
        "musicSelectToolbar has invalid mode, position, or position state");
    result.state = {};
    return result;
  }
  result.state.musicSelectToolbar.x = x->get<float>();
  result.state.musicSelectToolbar.y = y->get<float>();
  result.state.musicSelectToolbar.hasPosition = hasPosition->get<bool>();
  return result;
}

bool ApplicationUiStateStore::SaveAtomic(const std::filesystem::path &path,
                                         const ApplicationUiState &state,
                                         std::string &diagnostic) {
  if (Load(path).status == ApplicationUiStateLoadStatus::FutureVersion) {
    diagnostic = "application state was written by a newer version";
    return false;
  }
  const auto &toolbar = state.musicSelectToolbar;
  const json document = {
      {"schemaVersion", ApplicationUiState::kSchemaVersion},
      {"language", i18n::isLanguagePreference(state.language)
                       ? state.language : "system"},
      {"newcomerTutorialCompleted", state.newcomerTutorialCompleted},
      {"onlineDifficultyTablesRevision", state.onlineDifficultyTablesRevision},
      {"bundledDifficultyTablesRevision", state.bundledDifficultyTablesRevision},
      {"musicSelectToolbar",
       {{"mode", modeName(toolbar.mode)},
        {"x", toolbar.x},
        {"y", toolbar.y},
        {"hasPosition", toolbar.hasPosition}}},
  };
  return versioned_json::saveAtomic(path, document, diagnostic);
}
