"""Build a fixture from complete Chart Viewer and base cleanup methods."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    scene = (args.root / "src/scene/ChartViewerScene.cpp").read_text()
    base = (args.root / "src/scene/Scene.h").read_text()
    fixture = (args.root / "tests/chart_viewer_lifecycle_fixture.cpp").read_text()
    fixture = fixture.replace("BASE_METHODS", "\n".join(extract(base, signature) for signature in (
        "inline void cleanup()", "virtual ~Scene()", "void destroyOwnedViews()")))
    fixture = fixture.replace("VIEWER_METHODS", "\n".join(extract(scene, signature) for signature in (
        "ChartViewerScene::~ChartViewerScene()", "void ChartViewerScene::cleanupScene()")))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
