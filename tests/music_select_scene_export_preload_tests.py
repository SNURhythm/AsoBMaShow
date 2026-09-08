#!/usr/bin/env python3
import unittest
import subprocess

from tests import music_select_error_flow_contract_tests as scene_fixture

ROOT = scene_fixture.ROOT


def export_preload_fixture(source=None):
    if source is None:
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
    signatures = [
        "void MusicSelectScene::stopPreloadWorker()",
        "void MusicSelectScene::startPreloadForSelection()",
        "bool MusicSelectScene::reusePreloadedChart(",
        "void MusicSelectScene::refreshRepositoryRevisions()",
        "void MusicSelectScene::launchChartReplayExport(",
        "void MusicSelectScene::launchAutoPlayExport(",
        "void MusicSelectScene::applyRecordsExportResult()",
        "void MusicSelectScene::continueDirectoryRestore()",
        "void MusicSelectScene::tryCompletePendingPreloadLaunch()",
        "void MusicSelectScene::update(float)",
        "void MusicSelectScene::onPause()",
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
    def test_real_published_batch_stops_at_pause_and_rejects_inactive_launch(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        fixture = export_preload_fixture(source)
        fixture = fixture.replace(
            "void consumeActions() { ++inputConsumptions; if (actionHandoff) actionHandoff(); }",
            "void consumeActions();")
        fixture = fixture.replace("void launchSelected(bool, bool) {}",
                                  "void launchSelected(bool, bool);")
        methods = []
        for signature in ("void MusicSelectScene::consumeActions()",
                          "void MusicSelectScene::launchSelected(bool autoplay, bool practice)",
                          "void MusicSelectScene::onResume()"):
            method = signature + scene_fixture.function_body(source, signature)
            if "consumeActions" in signature:
                method = "#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1\n" + method + "\n#undef ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS\n"
            methods.append(method)
        fixture = fixture[:fixture.index("int main() {")] + "\n".join(methods) + r'''
int main() {
  MusicSelectScene scene;
  SceneManager manager;
  scene.context.sceneManager = &manager;
  manager.pause = [&] { scene.onPause(); };
  PublishedActions published;
  scene.skinSession_ = &published;
  scene.preloadedChart_ = std::make_unique<bms_parser::Chart>();
  scene.preloadedPath_ = scene.preloadedChart_->Meta.BmsPath;
  published.actions = {skin::Action{}, skin::Action{}};
  scene.consumeActions();
  const bool workerStarted = scene.launchThread_.joinable();
  if (workerStarted) scene.launchThread_.join();
  expect(manager.launches == 1 && scene.eventsDispatched == 1,
         "real published batch must stop dispatch immediately after synchronous pause");
  expect(!workerStarted && scene.context.jukebox.stops == 0 && scene.context.jukebox.loads == 0,
         "second Play must not stage shared audio or start a launch worker after handoff");
  scene.launchSelected(false, false);
  const bool inactiveWorker = scene.launchThread_.joinable();
  if (inactiveWorker) scene.launchThread_.join();
  expect(!inactiveWorker && scene.context.jukebox.stops == 0,
         "direct launch entry must reject inactive ownership");
  scene.onResume();
  scene.preloadedChart_ = std::make_unique<bms_parser::Chart>();
  scene.preloadedPath_ = scene.preloadedChart_->Meta.BmsPath;
  published.actions = {skin::Action{}};
  scene.consumeActions();
  expect(manager.launches == 2 && !scene.sceneActive_ && !scene.launchThread_.joinable(),
         "legitimate resume permits a fresh ready-chart launch");
  return failures == 0 ? 0 : 1;
}
'''
        self.run_fixture(fixture)

    def test_pending_handoff_stops_same_tick_hash_restore_and_shared_audio(self):
        fixture = export_preload_fixture()
        fixture = fixture[:fixture.index("int main() {")] + r'''
int main() {
  MusicSelectScene scene;
  SceneManager manager;
  scene.context.sceneManager = &manager;
  manager.pause = [&] { scene.onPause(); };
  ChartPreloadWorker worker(std::chrono::milliseconds(0));
  scene.preloadWorker_ = &worker;
  std::atomic_int audioLoads = 0;
  worker.configure([&](const ChartMetaRecord &, std::atomic_bool &) { ++audioLoads; });
  scene.preloadedChart_ = std::make_unique<bms_parser::Chart>();
  scene.preloadedPath_ = scene.preloadedChart_->Meta.BmsPath;
  scene.pendingLaunch_ = MusicSelectScene::PendingPreloadLaunch{ChartMetaRecord{}, true, false};
  scene.launching_ = true;
  scene.hashDirectoryOpen = true;
  ++scene.context.chartRepository.revision;
  scene.update(0);
  expect(manager.gameplay != nullptr, "pending preload must enter gameplay");
  expect(scene.directoryPublications == 0, "handoff must stop synchronous Hash restore/publication");
  expect(scene.inputConsumptions == 0, "handoff must stop input consumption");
  expect(scene.preloadedPath_.empty(), "same-tick revision must not queue shared audio after handoff");
  scene.bars_.rows.front().kind = skin::MusicSelectBarKind::Song;
  scene.startPreloadForSelection();
  worker.stop();
  expect(scene.preloadedPath_.empty(), "paused selector must not restart shared-audio staging");
  expect(audioLoads == 0, "no shared Jukebox load after handoff");
  scene.sceneActive_ = true;
  scene.update(0);
  expect(scene.directoryPublications > 0 && scene.inputConsumptions > 0,
         "active update must restore Hash directory and consume input");
  scene.stopPreloadWorker();
  scene.context.appInBackground = true;
  scene.startPreloadForSelection();
  expect(scene.preloadedPath_.empty(), "background selector must not restart shared audio");
  return failures == 0 ? 0 : 1;
}
'''
        self.run_fixture(fixture)

    def test_input_handoff_also_stops_remaining_update_work(self):
        fixture = export_preload_fixture()
        fixture = fixture[:fixture.index("int main() {")] + r'''
int main() {
  for (bool logical : {false, true}) {
    MusicSelectScene scene;
    auto &handoff = logical ? scene.logicalHandoff : scene.actionHandoff;
    handoff = [&] { scene.onPause(); };
    scene.update(0);
    expect(scene.inputConsumptions == (logical ? 1 : 2), "input handoff must stop further input consumption");
    expect(scene.directoryPublications == 0, "input handoff must stop remaining publication");
  }
  return failures == 0 ? 0 : 1;
}
'''
        self.run_fixture(fixture)

    def test_exports_exclusively_own_visuals_and_invalidate_reusable_preload(self):
        self.run_fixture(export_preload_fixture())

    def run_fixture(self, fixture):
        try:
            scene_fixture.MusicSelectSceneBehaviorTests().compile_and_run(fixture)
        except subprocess.CalledProcessError as error:
            self.fail(error.stderr)


if __name__ == "__main__":
    unittest.main()
