"""Exercise production Settings event handling and scene destruction without UI."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract
from result_skin_timeout_extract import without_includes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fixture = (args.root / "tests/settings_event_lifetime_fixture.cpp").read_text()
    fixture = f'#include "{(args.root / "src/ui/PresentationSchedule.h").as_posix()}"\n' + fixture
    for filename in ("src/storage/StoragePlatform.h", "src/storage/StoragePathIndex.h", "tests/StorageTestSupport.h"):
        fixture = f'#include "{(args.root / filename).as_posix()}"\n' + fixture
    fixture = fixture.replace("PRODUCTION_STORAGE_PATH_INDEX", without_includes(
        (args.root / "src/storage/StoragePathIndex.cpp").read_text()))
    platform = (args.root / "src/storage/StoragePlatform.cpp").read_text()
    inventory = (args.root / "src/storage/ChartResourceInventory.cpp").read_text()
    fixture = fixture.replace("PRODUCTION_STORAGE_PLATFORM", extract(
        platform, "StoragePlatformPaths MakeStoragePlatformPaths("))
    fixture = fixture.replace("PRODUCTION_SAFE_STORAGE_PATH", "\n".join(extract(inventory, signature) for signature in (
        "bool IsWithinFolder(", "bool IsSafeLocalPath(")))
    for marker, filename in (
        ("PRODUCTION_SCENE_HEADER", "src/scene/Scene.h"),
        ("PRODUCTION_MANAGER_HEADER", "src/scene/SceneManager.h"),
        ("PRODUCTION_MANAGER_METHODS", "src/scene/SceneManager.cpp"),
    ):
        fixture = fixture.replace(
            marker, without_includes((args.root / filename).read_text()))
    fixture = fixture.replace("PRODUCTION_SETTINGS_EVENTS", extract(
        (args.root / "src/scene/SettingsScene.cpp").read_text(),
        "EventHandleResult SettingsScene::handleEvents(SDL_Event &event)"))
    fixture = fixture.replace("PRODUCTION_RETURN_TARGET", without_includes(
        (args.root / "src/scene/SceneReturnTarget.h").read_text()))
    fixture = fixture.replace("PRODUCTION_RETURN_METHOD", without_includes(
        (args.root / "src/scene/SceneReturnTarget.cpp").read_text()))
    storage = (args.root / "src/scene/SettingsSceneStorage.cpp").read_text()
    fixture = fixture.replace("PRODUCTION_STORAGE_OPEN", extract(
        storage[storage.index("defer("):], "[this]"))
    integration = (args.root / "src/storage/StorageApplicationIntegration.cpp").read_text()
    fixture = fixture.replace("PRODUCTION_STORAGE_ACCESS", integration[
        integration.index("struct Access {"):integration.index("std::vector<std::filesystem::path> selectedRoots(")])
    roots_start = integration.index("      const StoragePathIndex allowed(")
    roots_end = integration.index("#if TARGET_OS_ANDROID", roots_start)
    fixture = fixture.replace("PRODUCTION_STORAGE_ROOT_ADMISSION", integration[roots_start:roots_end])
    admission = integration.split("*environment = std::move(*access);", 1)[1].split("    },", 1)[0]
    fixture = fixture.replace("PRODUCTION_MEDIA_ADMISSION", admission)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
