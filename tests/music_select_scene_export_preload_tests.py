#!/usr/bin/env python3
import unittest
import subprocess

from tests import music_select_error_flow_contract_tests as scene_fixture

ROOT = scene_fixture.ROOT


def export_preload_fixture():
    source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
    signatures = [
        "void MusicSelectScene::stopPreloadWorker()",
        "void MusicSelectScene::startPreloadForSelection()",
        "bool MusicSelectScene::reusePreloadedChart(",
        "void MusicSelectScene::refreshRepositoryRevisions()",
        "void MusicSelectScene::launchChartReplayExport(",
        "void MusicSelectScene::launchAutoPlayExport(",
        "void MusicSelectScene::applyRecordsExportResult()",
    ]
    methods = []
    for signature in signatures:
        start = source.index(signature)
        opening = source.index("{", start)
        methods.append(source[start:opening] + scene_fixture.function_body(source, signature))
    worker_header = (ROOT / "src/scene/ChartPreloadWorker.h").read_text()
    worker_source = (ROOT / "src/scene/ChartPreloadWorker.cpp").read_text()
    fixture = (ROOT / "tests/music_select_scene_export_preload_fixture.cpp").read_text()
    return (fixture.replace("SCENE_METHODS", "\n".join(methods))
            .replace("WORKER_DECLARATION", worker_header[worker_header.index("class ChartPreloadWorker"):])
            .replace("WORKER_METHODS", worker_source.replace('#include "ChartPreloadWorker.h"', "")))


class MusicSelectExportPreloadTests(unittest.TestCase):
    def test_exports_exclusively_own_visuals_and_invalidate_reusable_preload(self):
        try:
            scene_fixture.MusicSelectSceneBehaviorTests().compile_and_run(export_preload_fixture())
        except subprocess.CalledProcessError as error:
            self.fail(error.stderr)


if __name__ == "__main__":
    unittest.main()
