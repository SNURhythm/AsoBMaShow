"""Exercise retained music presentation and async completion with controlled I/O."""
import argparse
from pathlib import Path
import re

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    scene = (args.root / "src/scene/MainMenuScene.cpp").read_text()
    scene_header = (args.root / "src/scene/MainMenuScene.h").read_text()
    service = (args.root / "src/audio/MusicPlayerService.cpp").read_text()
    service_header = (args.root / "src/audio/MusicPlayerService.h").read_text()
    fixture = (args.root / "tests/music_player_status_fixture.cpp").read_text()
    status_type = re.search(r"^\s*([\w:]+) nativeControlStatusMessage;", service_header, re.M).group(1)
    menu_status_type = re.search(r"^\s*([\w:]+) musicStatusMessage;", scene_header, re.M).group(1)
    service_methods = "\n\n".join(extract(service, signature) for signature in (
        "void MusicPlayerService::PlaybackWorker(",
        "void MusicPlayerService::PublishNativeControlStatus(",
        "bool MusicPlayerService::ConsumeNativeControlStatus(\n    " + status_type + " &",
    ))
    scene_methods = "\n\n".join(extract(scene, signature) for signature in (
        "std::string formatMusicTime(",
        "std::string musicTrackDisplayName(",
        "std::string musicPlaylistTextSnapshot(",
        "void MainMenuScene::refreshMusicModal()",
        "void MainMenuScene::clearSavedMusicPlaylist()",
    ))
    fixture = fixture.replace("MENU_STATUS_TYPE", menu_status_type).replace("STATUS_TYPE", status_type)
    fixture = fixture.replace("SERVICE_METHODS", service_methods).replace("SCENE_METHODS", scene_methods)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
