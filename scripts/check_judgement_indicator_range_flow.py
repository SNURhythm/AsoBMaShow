#!/usr/bin/env python3

from pathlib import Path
import sys


def require(source: str, needle: str, label: str) -> None:
    if needle not in source:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> int:
    root = Path(sys.argv[1])
    controls = (root / "src/scene/SettingsSceneControls.cpp").read_text()
    settings_scene = (root / "src/scene/SettingsScene.cpp").read_text()
    preview = (root / "src/scene/SettingsScenePreview.cpp").read_text()
    renderer_header = (root / "src/scene/play/BMSRenderer.h").read_text()
    gameplay = (root / "src/scene/play/GamePlayScene.cpp").read_text()
    exporter = (root / "src/ReplayVideoExporter.cpp").read_text()
    replay_preflight = (
        root / "src/scene/play/ReplayVideoGameplayPreflight.cpp"
    ).read_text()

    require(controls, "syncJudgementIndicatorRangeInputText",
            "range input synchronization")
    require(controls, "commitJudgementIndicatorRangeInput",
            "range input commit")
    require(controls, "judgementIndicatorRangeMilliseconds",
            "range persistence")
    require(settings_scene, "summaryJudgementIndicatorRangeValueText",
            "summary range display")
    require(renderer_header, "int rangeMilliseconds",
            "required renderer range parameter")
    require(preview, "judgementIndicatorRangeMilliseconds",
            "settings preview propagation")
    require(gameplay, "judgementIndicatorRangeMilliseconds",
            "live gameplay propagation")
    require(replay_preflight,
            "PlayfieldPresentationConfig replayGameplayPresentationConfig",
            "shared replay presentation configuration")
    require(replay_preflight, "settings.judgementIndicatorRangeMilliseconds",
            "shared replay range propagation")
    require(exporter, "replay_video_export::replayGameplayPresentationConfig(",
            "single-stage replay range configuration")
    require(exporter,
            "configuration = replay_video_export::replayGameplayPresentationConfig(",
            "course replay range configuration")

    print("judgement indicator range flow audit passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
