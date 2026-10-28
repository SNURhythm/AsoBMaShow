"""Compile complete Music Player acquisition and release methods with effects."""
import argparse
from pathlib import Path
import re

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/MusicPlayerScene.cpp").read_text()
    menu = (args.root / "src/scene/MainMenuScene.cpp").read_text()
    slider = (args.root / "src/view/SnappedSlider.cpp").read_text()
    fixture_view = (args.root / "src/view/View.h").read_text()
    base = (args.root / "src/scene/Scene.h").read_text()
    fixture = (args.root / "tests/music_player_video_lifecycle_fixture.cpp").read_text()
    fixture = fixture.replace("BASE_METHODS", "\n".join(extract(base, signature) for signature in (
        "inline void cleanup()", "virtual ~Scene()", "void destroyOwnedViews()",
        "void clearPostedDeferred()", "virtual void onInputQueueOverflow()")))
    fixture = fixture.replace("POINTER_CANCEL_METHOD", extract(fixture_view, "void cancelPointerInput()"))
    fixture = fixture.replace("SLIDER_METHODS", "\n".join(extract(slider, signature) for signature in (
        "void SnappedSlider::cancelInteraction()", "void SnappedSlider::onPointerInputCancelled()")))
    fixture = fixture.replace("MENU_BACKGROUND_METHOD", extract(menu, "void MainMenuScene::onApplicationBackgroundChanged(bool background)"))
    cleanup = extract(source, "void MusicPlayerScene::cleanupScene()")
    # View implementations are controlled; keep every production cleanup handle.
    handles = re.findall(r"^  (\w+) = nullptr;", cleanup, re.MULTILINE)
    fixture = fixture.replace("VIEW_HANDLES", "\n".join(f"  View *{name} = nullptr;" for name in handles))
    fixture = fixture.replace("PLAYER_METHODS", "\n".join(extract(source, signature) for signature in (
        "MusicPlayerScene::~MusicPlayerScene()", "void MusicPlayerScene::watchVideo()",
        "void MusicPlayerScene::exitVideoFullscreen()", "void MusicPlayerScene::cleanupScene()",
        "void MusicPlayerScene::onPause()", "void MusicPlayerScene::onResume()",
        "void MusicPlayerScene::onInputQueueOverflow()",
        "void MusicPlayerScene::onApplicationBackgroundChanged(bool background)",
        "void MusicPlayerScene::setPlaybackRate(int percent)",
        "void MusicPlayerScene::persistPlaybackRate(bool updateStatusView)",
        "void MusicPlayerScene::setTabTopInset(float topInset)")))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
