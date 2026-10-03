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
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
