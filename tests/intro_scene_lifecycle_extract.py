"""Compile Intro input ownership and base cleanup with observed dependencies."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    scene = (args.root / "src/scene/IntroScene.cpp").read_text()
    base = (args.root / "src/scene/Scene.h").read_text()
    fixture = (args.root / "tests/intro_scene_lifecycle_fixture.cpp").read_text()
    fixture = fixture.replace("BASE_METHODS", "\n".join(extract(base, signature) for signature in (
        "inline void cleanup()", "virtual ~Scene()", "void destroyOwnedViews()")))
    # Preserve the original implicit destructor for the negative control.
    destructor = (extract(scene, "IntroScene::~IntroScene()")
                  if "IntroScene::~IntroScene()" in scene else "IntroScene::~IntroScene() = default;")
    fixture = fixture.replace("INTRO_METHODS", destructor + "\n" + "\n".join(
        extract(scene, signature) for signature in (
            "void IntroScene::cleanupScene()", "void IntroScene::startInputListening()",
            "void IntroScene::stopInputListening()")))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
