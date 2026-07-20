#!/usr/bin/env python3

from pathlib import Path
import sys


def require(source: str, needle: str, label: str) -> None:
    if needle not in source:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    root = Path(sys.argv[1])
    layout = (root / "src/scene/SettingsSceneLayout.cpp").read_text()
    controls = (root / "src/scene/SettingsSceneControls.cpp").read_text()
    settings_scene = (root / "src/scene/SettingsScene.cpp").read_text()
    renderer_header = (root / "src/scene/play/BMSRenderer.h").read_text()
    gameplay = (root / "src/scene/play/GamePlayScene.cpp").read_text()
    exporter = (root / "src/ReplayVideoExporter.cpp").read_text()

    require(layout, '"Indicator Range"', "summary range row")
    require(layout, '"Range (ms)"', "detailed range editor")
    require(layout, "commitJudgementIndicatorRangeInput",
            "detailed range commit callback")
    require(controls, "syncJudgementIndicatorRangeInputText",
            "range input synchronization")
    require(controls, "formatJudgementIndicatorRangeLabel",
            "range summary formatting")
    require(renderer_header, "int rangeMilliseconds",
            "required renderer range parameter")
    require(settings_scene, "judgementIndicatorRangeMilliseconds",
            "settings preview propagation")
    require(gameplay, "judgementIndicatorRangeMilliseconds",
            "live gameplay propagation")
    if exporter.count("settings.judgementIndicatorRangeMilliseconds") < 2:
        raise AssertionError(
            "single-stage and course replay exports must both propagate range"
        )

    print("judgement indicator range flow audit passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
