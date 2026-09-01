#include "ApplicationUiStateStore.h"

#include "VersionedJson.h"

#include <array>
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

  const auto toolbar = loaded.document.find("musicSelectToolbar");
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
  const auto &toolbar = state.musicSelectToolbar;
  const json document = {
      {"schemaVersion", ApplicationUiState::kSchemaVersion},
      {"musicSelectToolbar",
       {{"mode", modeName(toolbar.mode)},
        {"x", toolbar.x},
        {"y", toolbar.y},
        {"hasPosition", toolbar.hasPosition}}},
  };
  return versioned_json::saveAtomic(path, document, diagnostic);
}
