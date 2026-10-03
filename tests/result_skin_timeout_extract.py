"""Exercise production result rendering and scene ownership with fake graphics."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def without_includes(source):
    return "\n".join(
        line for line in source.splitlines()
        if not line.startswith(("#include", "#pragma"))
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fixture = (args.root / "tests/result_skin_timeout_fixture.cpp").read_text()
    for marker, filename in (
        ("PRODUCTION_SCENE_HEADER", "src/scene/Scene.h"),
        ("PRODUCTION_MANAGER_HEADER", "src/scene/SceneManager.h"),
        ("PRODUCTION_MANAGER_METHODS", "src/scene/SceneManager.cpp"),
    ):
        fixture = fixture.replace(
            marker, without_includes((args.root / filename).read_text()))
    fixture = fixture.replace("PRODUCTION_RESULT_RENDER", extract(
        (args.root / "src/scene/ResultScene.cpp").read_text(),
        "void ResultScene::renderScene()"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
