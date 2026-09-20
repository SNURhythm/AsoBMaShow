import subprocess
import unittest

from tests import music_select_error_flow_contract_tests as fixture_tools


ROOT = fixture_tools.ROOT


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    return source[start:opening] + fixture_tools.function_body(source, signature)


def records_fixture():
    source = (ROOT / "src/scene/MainMenuScene.cpp").read_text()
    modal = (ROOT / "src/scene/ReplayRecordsModal.cpp").read_text()
    methods = [method(source, signature) for signature in (
        "void MainMenuScene::startAutoPlayPlayback(",
        "void MainMenuScene::resetReplayWatchLoadingUi()",
        "void MainMenuScene::startReplayLoadWorker(",
        "void MainMenuScene::queueReplayLoadCompletion(",
        "void MainMenuScene::applyReplayLoadCompletion()",
        "void MainMenuScene::stopReplayLoadWorker()",
        "void MainMenuScene::stopReplayAndPreviewWork()",
        "MainMenuScene::~MainMenuScene()",
        "bool MainMenuScene::beginReplayExport(",
        "void MainMenuScene::applyReplayExportResult()",
        "void MainMenuScene::finishReplayResultRecallFailure(",
        "void MainMenuScene::finishRemoteResultRecallFailure(",
        "void MainMenuScene::startRemoteResultRecall(",
        "void MainMenuScene::onPause()",
        "void MainMenuScene::onResume()",
    )]
    for signature, success, prepared in (
        ("void MainMenuScene::startModernReplayResultRecall(",
         "queueReplayLoadCompletion([this, completion]() mutable",
         "auto completion = std::make_shared<ChartCompletion>();"),
        ("void MainMenuScene::startModernCourseReplayResultRecall(",
         "queueReplayLoadCompletion([this, session = std::move(session)]()",
         "auto session = std::make_shared<CoursePlaySession>();"),
    ):
        body = method(source, signature)
        worker = body.index("startReplayLoadWorker(")
        prefix = body[:worker]
        callback = fixture_tools.function_body(body, success)
        capture = body[body.index(success):body.index("{", body.index(success))]
        methods.append(prefix + '''
  startReplayLoadWorker([this](std::shared_ptr<std::atomic_bool> cancelled) {
    preparationEntered = true;
    while (!releasePreparation && !cancelled->load()) std::this_thread::yield();
    if (cancelled->load()) return;
    if (preparationFails) {
      queueReplayLoadCompletion([this] { finishReplayResultRecallFailure("unavailable"); });
      return;
    }
    ''' + prepared + "\n" + capture + callback + ");\n  });\n}")
    modal_methods = [method(modal, signature) for signature in (
        "void ReplayRecordsModal::hide()",
        "void ReplayRecordsModal::setLoadInProgress(",
        "void ReplayRecordsModal::setResultRecallInProgress(",
        "void ReplayRecordsModal::setExportInProgress(",
        "bool ReplayRecordsModal::operationInProgress()",
        "bool ReplayRecordsModal::canHide()",
    )]
    callbacks = []
    for name, signature in (
        ("watchAutoPlay", "const ChartMetaRecord &record"),
        ("recallModernChart", "const ChartMetaRecord &record, const ModernChartResultRecord &modern"),
        ("recallModernCourse", "const ModernCourseResultRecord &modern, bool retrySame"),
        ("recallRemote", "const IrRemoteRecordId &identity, const std::string &selectedStableKey"),
    ):
        callbacks.append(f"callbacks.{name} = [this]({signature}) " +
                         fixture_tools.function_body(source, f"callbacks.{name} =") + ";")
    fixture = (ROOT / "tests/main_menu_records_lifecycle_fixture.cpp").read_text()
    return (fixture.replace("REPOSITORY_ROOT", ROOT.as_posix()).replace("OWNER_METHODS", "\n".join(methods))
            .replace("MODAL_METHODS", "\n".join(modal_methods))
            .replace("OWNER_CALLBACKS", "\n".join(callbacks)))


class MainMenuRecordsLifecycleTests(unittest.TestCase):
    def test_resume_activates_selected_music_select_skin_after_callback_returns(self):
        source = (ROOT / "src/scene/MainMenuScene.cpp").read_text()
        fixture = (ROOT / "tests/main_menu_skin_resume_fixture.cpp").read_text()
        fixture = fixture.replace("REPOSITORY_ROOT", ROOT.as_posix()).replace(
            "RESUME_BODY", fixture_tools.function_body(source, "void MainMenuScene::onResume()"))
        for enabled in (0, 1):
            with self.subTest(skins_enabled=enabled):
                try:
                    fixture_tools.MusicSelectSceneBehaviorTests().compile_and_run(
                        f"#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS {enabled}\n" + fixture)
                except subprocess.CalledProcessError as error:
                    self.fail(error.stderr)

    def test_actual_owner_callbacks_release_records_on_return_and_cancel(self):
        try:
            fixture_tools.MusicSelectSceneBehaviorTests().compile_and_run(records_fixture(), [ROOT / "src/replay/ReplayExportJob.cpp", ROOT / "src/scene/ReplayRecordTask.cpp", ROOT / "src/scene/FindBmsTask.cpp", ROOT / "tests/support/AllocationFailure.cpp"])
        except subprocess.CalledProcessError as error:
            self.fail(error.stderr)


if __name__ == "__main__":
    unittest.main()
