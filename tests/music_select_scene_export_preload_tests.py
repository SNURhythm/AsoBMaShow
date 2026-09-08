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
void runVolumeCases() {
  for (int channel : {17, 18, 19}) {
    for (int mode : {0, 1, 2, 3, 4, 5}) {
      caseName = "volume/" + std::to_string(channel) + "/" + std::to_string(mode);
      MusicSelectScene scene;
      SceneManager manager;
      PublishedActions published;
      scene.context.sceneManager = &manager;
      scene.skinSession_ = &published;
      auto volume = [&](double value) {
        return skin::Action{skin::MusicSelectSkinActionKind::FloatWriter, channel, value};
      };
      auto channelValue = [&](const Context::AudioSettings &settings) {
        return channel == 17 ? settings.masterVolume :
               channel == 18 ? settings.keysoundVolume : settings.bgmVolume;
      };
      const double accepted = mode == 2 ? 1.0 : 0.25;
      const int commits = mode == 2 ? 0 : 1;
      manager.pause = [&] {
        expect(scene.sceneActive_, "audio commit must precede actual pause");
        expect(scene.context.audioDeviceManager.applied.size() == commits &&
                   scene.context.savedAudio.size() == commits,
               "accepted volume must apply and save before pause");
        if (commits && !scene.context.audioDeviceManager.applied.empty() &&
            !scene.context.savedAudio.empty()) {
          const auto &applied = scene.context.audioDeviceManager.applied.back();
          const auto &saved = scene.context.savedAudio.back();
          expect(channelValue(applied) == accepted && channelValue(saved) == accepted,
                 "runtime and persistence must receive final coalesced channel value");
          expect(applied.masterVolume == saved.masterVolume &&
                     applied.keysoundVolume == saved.keysoundVolume &&
                     applied.bgmVolume == saved.bgmVolume,
                 "all applied channels must match saved channels");
          expect(applied.masterVolume == (channel == 17 || mode == 5 ? accepted : 1.0) &&
                     applied.keysoundVolume == (channel == 18 || mode == 5 ? accepted : 1.0) &&
                     applied.bgmVolume == (channel == 19 || mode == 5 ? accepted : 1.0),
                 "commit must preserve untouched channels and coalesce mixed writers");
        }
        scene.context.audioEvents.push_back("pause");
        scene.onPause();
      };
      scene.preloadedChart_ = std::make_unique<bms_parser::Chart>();
      scene.preloadedPath_ = scene.preloadedChart_->Meta.BmsPath;
      if (mode == 3) published.actions.push_back(volume(0.5));
      if (mode == 5) {
        for (int mixedChannel : {17, 18, 19}) {
          published.actions.push_back(
              {skin::MusicSelectSkinActionKind::FloatWriter, mixedChannel, accepted});
        }
      }
      published.actions.push_back(volume(accepted));
      if (mode == 3) published.actions.push_back(volume(accepted));
      if (mode != 4) published.actions.push_back(skin::Action{});
      if (mode == 1) {
        published.actions.push_back(volume(0.75));
        published.actions.push_back(skin::Action{});
      }
      scene.consumeActions();
      const bool workerStarted = scene.launchThread_.joinable();
      if (workerStarted) scene.launchThread_.join();
      expect(!workerStarted && scene.context.jukebox.stops == 0 &&
                 scene.context.jukebox.loads == 0,
             "batch must not start audio staging or launch worker after pause");
      expect(channelValue(scene.context.settings.audioVideo.audio) == accepted,
             "trailing write must not mutate settings");
      std::vector<std::string> expected;
      if (commits) expected = {"apply", "save"};
      if (mode != 4) expected.push_back("pause");
      expect(scene.context.audioEvents == expected,
             "commit must coalesce before pause with no post-pause apply/save");
      expect(manager.launches == (mode == 4 ? 0 : 1) &&
                 scene.eventsDispatched == (mode == 4 ? 0 : 1),
             "batch must perform exactly one requested handoff");
      if (mode != 4) scene.onResume();
      scene.preloadedChart_ = std::make_unique<bms_parser::Chart>();
      scene.preloadedPath_ = scene.preloadedChart_->Meta.BmsPath;
      published.actions = {volume(accepted), skin::Action{}};
      scene.consumeActions();
      expected.push_back("pause");
      expect(scene.context.audioEvents == expected && !scene.sceneActive_ &&
                 manager.launches == (mode == 4 ? 1 : 2) &&
                 !scene.launchThread_.joinable(),
             "resume must permit fresh launch without replay or redundant audio commit");
    }
  }
}

int main() {
  runVolumeCases();
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
