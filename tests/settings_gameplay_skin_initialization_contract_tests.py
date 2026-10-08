#!/usr/bin/env python3
"""Regression contract for retained Gameplay Skins tab initialization."""

from pathlib import Path
import re
import subprocess
import tempfile

try:
    from .gameplay_terminal_scene_extract import extract
    from .support.fixture_compiler import FixtureCompiler
except ImportError:
    from gameplay_terminal_scene_extract import extract
    from support.fixture_compiler import FixtureCompiler
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SettingsGameplaySkinInitializationContracts(unittest.TestCase):
    def test_ios_skin_storage_identity_is_independent_of_language(self) -> None:
        settings = (ROOT / "src/scene/SettingsSceneSkins.cpp").read_text()
        begin = settings.index("  const std::filesystem::path visibleSkinRoot =")
        end = settings.index("  overview->addView", begin)
        storage = settings[begin:end]
        utils = (ROOT / "src/Utils.cpp").read_text()
        relative = extract(utils, "std::filesystem::path Utils::GetStoragePathRelativeToDocuments(")
        utf8 = extract(utils, "std::string Utils::GetStoragePathUtf8RelativeToDocuments(")
        fixture = r'''
#include "i18n/Localization.h"
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <cassert>
#define TARGET_OS_IOS 1
struct Utils {
  static std::filesystem::path GetDocumentsPath(const std::filesystem::path &subpath) {
    return std::filesystem::path("/container/Documents") / subpath;
  }
  static std::filesystem::path GetStoragePathRelativeToDocuments(
      const std::filesystem::path &, const std::filesystem::path &);
  static std::string GetStoragePathUtf8RelativeToDocuments(
      const std::filesystem::path &, const std::filesystem::path &);
};
std::string fspath_to_path_t(const std::filesystem::path &path) { return path.generic_string(); }
std::string path_t_to_utf8(const std::string &path) { return path; }
// RELATIVE_METHODS
struct Roots { std::filesystem::path visiblePackages; };
std::pair<std::filesystem::path, std::string> skinLocation(std::optional<Roots> roots) {
  struct { std::optional<Roots> skinStorageRoots; } context{std::move(roots)};
  // SETTINGS_STORAGE
  return {visibleSkinRoot, visibleSkinFolder};
}
int main() {
  for (const auto language : {i18n::Language::English, i18n::Language::Korean,
                              i18n::Language::Japanese}) {
    i18n::setLanguage(language);
    const std::filesystem::path canonical = "/container/Documents/Skins";
    const auto configured = skinLocation(Roots{canonical});
    assert(configured.first == canonical && configured.second == "Documents/Skins");
    const auto fallback = skinLocation(std::nullopt);
    assert(fallback.first == canonical && fallback.second == "Documents/Skins");
    const auto nested = skinLocation(Roots{canonical / "custom"});
    assert(nested.first == canonical / "custom" && nested.second == "custom");
    const auto external = skinLocation(Roots{"/external/Skins"});
    assert(external.first == "/external/Skins" && external.second == "/external/Skins");
  }
}
'''
        fixture = fixture.replace("// RELATIVE_METHODS", relative + "\n" + utf8)
        fixture = fixture.replace("// SETTINGS_STORAGE", storage)
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "skin_storage.cpp"
            source.write_text(fixture)
            binary = directory / ("skin_storage" + compiler.executable_suffix)
            compiler.build([source, ROOT / "src/i18n/Localization.cpp"], binary,
                           directory, includes=[ROOT / "src"])
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_scratchless_skin_callbacks_keep_mode_and_multiple_tables_independent(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneControls.cpp").read_text()
        controls = extract(source, "void SettingsScene::appendScratchlessModeSettings(")
        mode_callback = extract(controls, "[this, keyMode](const std::string &id)")
        table_callback = extract(controls, "[this, keyMode, url = table.sourceUrl, content]()")
        fixture = r'''
#include "AppSettings.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <string>
#include <vector>
struct Checkbox {
  bool checked = false;
  void setChecked(bool value) { checked = value; }
};
struct Table { std::string sourceUrl; };
struct SettingsScene {
  struct { AppSettings settings; } context;
  int lastLayoutWidth = 100;
  int saves = 0;
  void persistSettings() { ++saves; }
  std::function<void(const std::string &)> modeCallback(int keyMode) {
    return MODE_CALLBACK;
  }
  std::function<void()> tableCallback(int keyMode, const Table &table, Checkbox *content) {
    return TABLE_CALLBACK;
  }
};
int main() {
  SettingsScene scene;
  const auto original5 = scene.context.settings.scratchless5K;
  const auto original7 = scene.context.settings.scratchless7K;
  auto mode5 = scene.modeCallback(-5);
  auto mode7 = scene.modeCallback(-7);
  mode5("0");
  mode7("1");
  assert(scene.context.settings.scratchless5K.mode == AppSettings::ScratchlessMode::Disabled);
  assert(scene.context.settings.scratchless7K.mode == AppSettings::ScratchlessMode::Enabled);
  assert(scene.lastLayoutWidth == -1);
  mode5("2");
  assert(scene.context.settings.scratchless7K.mode == AppSettings::ScratchlessMode::Enabled);
  Checkbox add7To5, extra5, remove7;
  auto add7 = scene.tableCallback(-5, {original7.tableUrls.front()}, &add7To5);
  auto addExtra = scene.tableCallback(-5, {"https://example.com/another.json"}, &extra5);
  auto toggle7 = scene.tableCallback(-7, {original7.tableUrls.front()}, &remove7);
  add7();
  addExtra();
  assert(add7To5.checked && extra5.checked);
  assert(scene.context.settings.scratchless5K.tableUrls.size() == 3);
  assert(scene.context.settings.scratchless7K.tableUrls == original7.tableUrls);
  toggle7();
  assert(!remove7.checked && scene.context.settings.scratchless7K.tableUrls.empty());
  assert(scene.context.settings.scratchless5K.tableUrls.size() == 3);
  add7();
  assert(!add7To5.checked && extra5.checked);
  assert(scene.context.settings.scratchless5K.tableUrls ==
         (std::vector<std::string>{original5.tableUrls.front(), "https://example.com/another.json"}));
  assert(scene.saves == 7);
}
'''
        fixture = fixture.replace("MODE_CALLBACK", mode_callback).replace("TABLE_CALLBACK", table_callback)
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source_path = directory / "scratchless_skin_callbacks.cpp"
            source_path.write_text(fixture)
            binary = directory / ("scratchless_skin_callbacks" + compiler.executable_suffix)
            compiler.build([source_path, ROOT / "src/settings/AudioVideoSettings.cpp"], binary,
                           directory, includes=[ROOT / "src"])
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_open_skins_tab_refreshes_table_choices_on_library_changes(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneTables.cpp").read_text()
        refresh = extract(source, "void SettingsScene::refreshTablesIfLibraryChanged()")
        fixture = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
enum class SettingsTab { GameplaySkins, DifficultyTables, BmsLibrary, Misc };
struct Repository {
  std::uint64_t revision = 0;
  std::vector<std::string> tables;
  std::uint64_t GetLibraryRevision() { return revision; }
};
struct SettingsScene {
  struct { Repository chartRepository; } context;
  SettingsTab activeTab = SettingsTab::GameplaySkins;
  std::uint64_t observedLibraryRevision = 0;
  int lastLayoutWidth = 800;
  std::vector<std::string> difficultyTables;
  void loadDifficultyTables() { difficultyTables = context.chartRepository.tables; }
  void loadChartEntries() {}
  void refreshChartEntryBackupStatuses() {}
  void refreshTablesIfLibraryChanged();
};
REFRESH_METHOD
int main() {
  SettingsScene scene;
  scene.context.chartRepository.tables = {"new table"};
  scene.context.chartRepository.revision = 1;
  scene.refreshTablesIfLibraryChanged();
  assert(scene.difficultyTables == std::vector<std::string>{"new table"});
  assert(scene.lastLayoutWidth == -1);
  scene.lastLayoutWidth = 800;
  scene.refreshTablesIfLibraryChanged();
  assert(scene.lastLayoutWidth == 800);
  scene.context.chartRepository.tables.clear();
  scene.context.chartRepository.revision = 2;
  scene.refreshTablesIfLibraryChanged();
  assert(scene.difficultyTables.empty() && scene.lastLayoutWidth == -1);
}
'''
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source_path = directory / "scratchless_table_refresh.cpp"
            source_path.write_text(fixture.replace("REFRESH_METHOD", refresh))
            binary = directory / ("scratchless_table_refresh" + compiler.executable_suffix)
            compiler.build([source_path], binary, directory)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_retained_gameplay_skins_tab_refreshes_before_first_layout(self) -> None:
        source = (ROOT / "src/scene/SettingsScene.cpp").read_text(encoding="utf-8")
        init = re.search(
            r"void SettingsScene::init\(\) \{(?P<body>[\s\S]*?)\n\}", source
        )
        self.assertIsNotNone(init)
        body = init.group("body")
        self.assertIn(
            "updateGameplaySkinSettingsController();",
            body,
            "The retained Gameplay Skins tab must refresh its controller "
            "before its initial layout.",
        )
        refresh = body.index("updateGameplaySkinSettingsController();")
        first_layout = body.index("ensureLayoutUpToDate();")
        self.assertLess(
            refresh,
            first_layout,
            "The retained Gameplay Skins tab must build from a refreshed "
            "controller snapshot on its first frame.",
        )

    def test_layout_reset_clears_every_retained_gameplay_skin_view(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneLayout.cpp").read_text(
            encoding="utf-8"
        )
        reset = re.search(
            r"void SettingsScene::resetViewState\(\) \{(?P<body>[\s\S]*?)\n\}",
            source,
        )
        self.assertIsNotNone(reset)
        body = reset.group("body")
        for pointer in (
            "gameplaySkinStatusText",
            "gameplaySkinUiMessageText",
            "gameplaySkinConfigurationDigestText",
            "gameplaySkinSafetyOverlayRoot",
            "gameplaySkinBusyOverlayRoot",
            "gameplaySkinBusyOverlayStatusText",
            "gameplaySkinBusyOverlayCancelButton",
        ):
            self.assertIn(
                f"{pointer} = nullptr;",
                body,
                f"{pointer} must not outlive a deleted settings view tree",
            )

    def test_busy_rebuild_reenables_ordinary_controls_when_idle(self) -> None:
        source = (ROOT / "src/scene/SettingsSceneSkins.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "gameplaySkinControlsBuiltDisabled = !ordinaryActionsEnabled;",
            source,
            "the tab must remember when a busy rebuild disables controls",
        )
        self.assertRegex(
            source,
            r"gameplaySkinControlsBuiltDisabled\s*&&\s*"
            r"skin::gameplaySkinSettingsActionAvailability\(snapshot\)"
            r"\.ordinaryActions",
            "returning to an idle snapshot must request one control rebuild",
        )

if __name__ == "__main__":
    unittest.main()
