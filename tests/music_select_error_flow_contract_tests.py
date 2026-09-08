#!/usr/bin/env python3
import unittest
import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def fixture_compile_command(compiler, frontend, compiler_id, source, executable,
                            extra_sources=()):
    if frontend == "MSVC" or compiler_id == "MSVC":
        objects = (f"/Fo{source.parent}{os.sep}" if extra_sources
                   else f"/Fo{source.with_suffix('.obj')}")
        return [compiler, "/nologo", "/std:c++20", "/EHsc", str(source),
                *map(str, extra_sources), objects, f"/Fe{executable}"]
    return [compiler, "-std=c++20", "-pthread", str(source),
            *map(str, extra_sources), "-o", str(executable)]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class MusicSelectErrorFlowContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src/scene/MusicSelectSkinErrorScene.cpp").read_text(
            encoding="utf-8"
        )

    def test_settings_from_skin_error_returns_to_intro(self):
        body = function_body(
            self.source, "void MusicSelectSkinErrorScene::openSettings()"
        )
        self.assertIn('SceneReturnTarget::Registered("Intro")', body)
        self.assertNotIn("SceneReturnTarget::Retained", body)
        self.assertNotIn(
            ")), true)",
            body,
            "an error scene returning to Intro must not remain backgrounded",
        )

    def test_skin_error_offers_back_to_intro(self):
        initialization = function_body(
            self.source, "void MusicSelectSkinErrorScene::init()"
        )
        self.assertIn('makeButton("Back")', initialization)
        self.assertIn('changeScene("Intro")', initialization)


class MusicSelectSceneBehaviorTests(unittest.TestCase):
    def test_records_autoplay_audio_failure_cancel_and_retry(self):
        self.run_replay_audio_fixture([4])

    def test_replay_audio_failure_cancel_and_success_preserve_launch_options(self):
        self.run_replay_audio_fixture(range(4))

    def run_replay_audio_fixture(self, paths):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        signatures = [
            "void MusicSelectScene::launchCourseReplay(const MusicSelectBar &course, int slot, const MusicSelectBarManagerReadView &snapshot)",
            "void MusicSelectScene::launchSelectedReplay(int slot)",
            "void MusicSelectScene::launchChartReplay(const ChartMetaRecord &record, const ModernChartResultRecord &modern, bool ghostBattle)",
            "void MusicSelectScene::launchAutoPlay(const ChartMetaRecord &record)",
        ]
        methods = "\n".join(signature + function_body(source, signature.split("(")[0] + "(")
                            for signature in signatures)
        fixture = (ROOT / "tests/music_select_scene_replay_audio_fixture.cpp").read_text()
        callback = function_body(source, "callbacks.watchAutoPlay =")
        for path in paths:
            with self.subTest(path=path):
                self.compile_and_run(fixture.replace("REPOSITORY_ROOT", ROOT.as_posix())
                                     .replace("SCENE_METHODS", methods)
                                     .replace("AUTOPLAY_CALLBACK", callback)
                                     .replace("SCENE_TEST", "testAutoPlayAudio()" if path == 4
                                              else f"testReplayAudio({path})"))

    def test_folder_statistics_prioritize_selection_and_survive_navigation(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        method = function_body(source, "void MusicSelectScene::requestFolderStatus(")
        prefix = method[1:method.index("const int longNoteMode")]
        fixture = (ROOT / "tests/music_select_folder_status_fixture.cpp").read_text()
        self.compile_and_run(
            fixture.replace("REPOSITORY_ROOT", ROOT.as_posix())
                   .replace("SCENE_REQUEST_PREFIX", prefix),
            [ROOT / "src/music_select/MusicSelectFolderStatusLoader.cpp",
             ROOT / "src/music_select/MusicSelectBarManager.cpp"])

    def test_course_audio_failure_blocks_gameplay_and_allows_successful_retry(self):
        self.run_course_audio_fixture("testCourseAudioFailure(false)")

    def test_folder_autoplay_audio_failure_blocks_gameplay_and_allows_successful_retry(self):
        self.run_course_audio_fixture("testCourseAudioFailure(true)")

    def test_cancelled_course_audio_still_blocks_gameplay(self):
        self.run_course_audio_fixture("testCancelledCourseAudio()")

    def test_course_real_worker_cancellation_and_stale_deferred_completion(self):
        self.run_course_audio_fixture("testAsyncCourseLifecycle()")

    def test_folder_autoplay_real_worker_cancellation_and_stale_deferred_completion(self):
        self.run_course_audio_fixture("testAsyncCourseLifecycle(true)")

    def test_course_real_worker_captures_options_and_recovers_from_parse_failure(self):
        self.run_course_audio_fixture("testAsyncCourseOptionsAndParseRetry()")

    def test_course_deferred_failure_does_not_revive_background_audio(self):
        self.run_course_audio_fixture("testCourseFailureWhileApplicationBackgrounded()")

    def run_course_audio_fixture(self, test_name):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        signatures = [
            "void MusicSelectScene::launchCourse(const MusicSelectBar &bar, bool autoplay)",
            "void MusicSelectScene::launchDirectoryAutoplay(const MusicSelectBar &directory)",
            "void MusicSelectScene::onPause()",
            "void MusicSelectScene::onResume()",
        ]
        methods = "\n".join(
            signature + function_body(source, signature.split("(")[0] + "(")
            for signature in signatures)
        fixture = (ROOT / "tests/music_select_scene_course_audio_fixture.cpp").read_text()
        cleanup = function_body(source, "void MusicSelectScene::cleanupScene()")
        cleanup = cleanup[1:cleanup.index("stopPreloadWorker();")]
        self.compile_and_run(fixture.replace("REPOSITORY_ROOT", ROOT.as_posix())
                             .replace("CLEANUP_LAUNCH", cleanup)
                             .replace("SCENE_METHODS", methods)
                             .replace("SCENE_TEST", test_name))

    def test_runtime_error_keyboard_and_controller_settings_recovery(self):
        self.run_error_recovery_fixture("testSettingsRecovery")

    def test_runtime_error_escape_and_controller_cancel_return_to_intro(self):
        self.run_error_recovery_fixture("testBackRecovery")

    def test_runtime_error_modal_blocks_selector_but_preserves_pointer_recovery(self):
        self.run_error_recovery_fixture("testErrorModalIsolation")

    def test_healthy_settings_still_retains_selector(self):
        self.run_error_recovery_fixture("testHealthySettingsRetainsSelector")

    def run_error_recovery_fixture(self, test_name):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        events = function_body(source, "EventHandleResult MusicSelectScene::handleEvents(")
        prefix = events[1:events.index("if (selectorInputBlocked())")]
        signatures = [
            "void MusicSelectScene::enterError(std::vector<skin::SkinDiagnostic> diagnostics)",
            "void MusicSelectScene::openSettings()",
        ]
        methods = "\n".join(
            signature + function_body(source, signature.split("(")[0] + "(")
            for signature in signatures)
        fixture = (ROOT / "tests/music_select_scene_error_recovery_fixture.cpp").read_text()
        fixture = (fixture.replace("REPOSITORY_ROOT", ROOT.as_posix())
                   .replace("SCENE_METHODS", methods)
                   .replace("ERROR_EVENT_PREFIX", prefix)
                   .replace("SCENE_TEST", test_name))
        for enabled in (0, 1):
            with self.subTest(lua_enabled=enabled):
                self.compile_and_run(
                    f"#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS {enabled}\n" + fixture)

    def test_uncached_launch_cleanup_cancels_parser_and_audio_without_handoff(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        launch = function_body(source, "void MusicSelectScene::launchSelected(")
        worker = launch[launch.index("if (launchThread_.joinable())"):-1]
        cleanup = function_body(source, "void MusicSelectScene::cleanupScene()")
        cleanup = cleanup[1:cleanup.index("stopPreloadWorker();")]
        fixture = (ROOT / "tests/music_select_scene_launch_cancel_fixture.cpp").read_text()
        self.compile_and_run(fixture.replace("LAUNCH_WORKER", worker)
                             .replace("CLEANUP_LAUNCH", cleanup))

    def test_failed_fallback_audio_load_does_not_launch_gameplay(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        launch = function_body(source, "void MusicSelectScene::launchSelected(")
        worker = launch[launch.index("launchThread_ = std::jthread("):]
        start = worker.index("context.jukebox.stop();")
        finish = worker.index("postDeferred(", start)
        fixture = (ROOT / "tests/music_select_scene_launch_audio_fixture.cpp").read_text()
        self.compile_and_run(fixture.replace("STAGING_BLOCK", worker[start:finish]))

    def test_recursive_directory_queries_preserve_persisted_folder_metadata(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        method = function_body(source, "bool MusicSelectScene::loadDirectoryChildren(")
        branch = function_body(method, "case skin::MusicSelectBarKind::Folder:")
        fixture = (ROOT / "tests/music_select_scene_directory_metadata_fixture.cpp").read_text()
        self.compile_and_run(fixture.replace("SCENE_FOLDER_BRANCH", branch))

    def test_async_directory_autoplay_completes_without_reloading(self):
        self.run_directory_loading_fixture("testAutoplayCompletion")

    def test_async_directory_pointer_targets_clicked_folder(self):
        self.run_directory_loading_fixture("testPointerTargetsClickedFolder")

    def test_empty_category_autoplay_keeps_directory_reloadable(self):
        self.run_directory_loading_fixture("testEmptyCategoryAutoplay")

    def test_async_directory_request_honors_latest_autoplay_intent(self):
        self.run_directory_loading_fixture("testLatestRequestIntent")

    def test_async_restore_preserves_pending_target_across_revisions(self):
        self.run_directory_loading_fixture("testRestoreSurvivesAnotherRevision")

    def test_foreground_resumes_background_directory_restore(self):
        self.run_directory_loading_fixture("testForegroundResumesDirectoryRestore")

    def test_failed_scene_cancels_ready_directory_autoplay(self):
        self.run_directory_loading_fixture("testFailedSceneCancelsReadyAutoplay")

    def test_library_reload_replaces_rows_before_configuring(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        body = function_body(source, "void MusicSelectScene::reloadLibrary(")
        self.assertLess(
            body.index("bars_.refresh("), body.index("bars_.configure("),
            "reload must discard the old provider before configuration can rebuild its index",
        )

    def run_directory_loading_fixture(self, test_name):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        signatures = [
            "void MusicSelectScene::requestDirectoryLoad(",
            "void MusicSelectScene::applyDirectoryLoads()",
            "void MusicSelectScene::cancelDirectoryLoad()",
            "void MusicSelectScene::continueDirectoryRestore()",
            "bool MusicSelectScene::openDirectory(",
            "void MusicSelectScene::launchSelectedDirectoryAutoplay()",
            "void MusicSelectScene::applySkinPointerResult(",
            "void MusicSelectScene::onApplicationBackgroundChanged(",
        ]
        optional_helper = "void MusicSelectScene::launchDirectoryAutoplay("
        if optional_helper in source:
            signatures.append(optional_helper)
        methods = []
        for signature in signatures:
            start = source.index(signature)
            opening = source.index("{", start)
            methods.append(source[start:opening] + function_body(source, signature))
        moved = function_body(source, "void MusicSelectScene::selectedBarMoved()")
        guard_start = moved.index("if (directoryRequest_ &&")
        guard_end = moved.index("requestFolderStatus(snapshot);", guard_start)
        reload = function_body(source, "void MusicSelectScene::reloadLibrary(")
        capture = reload[1:reload.index("const std::uint64_t loadedRevision")]
        restore = reload[reload.rindex("if (preserveDirectory)"):]
        restore = "if (preserveDirectory) " + function_body(
            restore, "if (preserveDirectory)")
        fixture = (ROOT / "tests/music_select_scene_directory_loading_fixture.cpp").read_text()
        fixture = (fixture.replace("REPOSITORY_ROOT", ROOT.as_posix())
                   .replace("SCENE_METHODS", "\n".join(methods))
                   .replace("SELECTED_MOVE_GUARD", moved[guard_start:guard_end])
                   .replace("RELOAD_CAPTURE", capture)
                   .replace("RELOAD_RESTORE", restore)
                   .replace("SCENE_TEST", test_name))
        try:
            self.compile_and_run(
                fixture, [ROOT / "src/music_select/MusicSelectBarManager.cpp"])
        except subprocess.CalledProcessError as error:
            self.fail(error.stderr)

    def test_sound_services_follow_changed_paths_and_bookmarks(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        signatures = [
            "std::filesystem::path musicSelectSoundSetRoot(const std::string &configured)",
            "void MusicSelectScene::configureSoundServices()",
        ]
        methods = "\n".join(signature + function_body(source, signature)
                            for signature in signatures)
        fixture = (ROOT / "tests/music_select_scene_sound_settings_fixture.cpp").read_text()
        for ios in (0, 1):
            self.compile_and_run(f"#define TARGET_OS_IOS {ios}\n" +
                                 fixture.replace("SCENE_METHODS", methods))

    def test_scene_ranking_cache_evicts_oldest_updates_at_capacity(self):
        self.run_scene_fixture("music_select_scene_ranking_cache_fixture.cpp", [
            "void MusicSelectScene::updateRanking()",
        ])

    def test_launch_completion_uses_ui_owned_deferred_queue(self):
        header = (ROOT / "src/scene/Scene.h").read_text()
        header = "\n".join(line for line in header.splitlines()
                           if not line.startswith(("#include", "#pragma")))
        scene = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        launch = function_body(scene, "void MusicSelectScene::launchSelected(")
        worker = launch[launch.index("launchThread_ = std::jthread("):]
        submit = "postDeferred(callback)" if "postDeferred(" in worker else "defer(callback, 0, true)"
        self.assertEqual(worker.count("defer("), 0 if "postDeferred(" in worker else 2)
        fixture = (ROOT / "tests/scene_deferred_fixture.cpp").read_text()
        self.compile_and_run(fixture.replace("SCENE_HEADER", header)
                             .replace("SUBMIT_CALLBACK", submit))

    def test_failed_audio_preload_is_not_published(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        signature = "[this](const ChartMetaRecord &request, std::atomic_bool &cancelled)"
        callback = signature + function_body(source, signature)
        fixture = (ROOT / "tests/music_select_scene_preload_fixture.cpp").read_text()
        self.compile_and_run(fixture.replace("PRELOAD_CALLBACK", callback))

    def test_background_audio_resumes_only_for_active_foreground_selector(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        signatures = ["void MusicSelectScene::onPause()",
                      "void MusicSelectScene::onResume()"]
        methods = "\n".join(signature + function_body(source, signature)
                            for signature in signatures)
        background = "void MusicSelectScene::onApplicationBackgroundChanged(bool background)"
        methods += "\n" + background + (function_body(source, background)
                                        if background in source else " {}")
        fixture = (ROOT / "tests/music_select_scene_background_fixture.cpp").read_text()
        for enabled in (0, 1):
            self.compile_and_run(
                f"#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS {enabled}\n"
                + fixture.replace("SCENE_METHODS", methods))

    def test_fixture_uses_configured_msvc_and_clang_cl_frontends(self):
        for compiler_id in ("MSVC", "Clang"):
            command = fixture_compile_command(
                "C:/Program Files/compiler.exe", "MSVC", compiler_id,
                Path("scene.cpp"), Path("scene.exe"))
            self.assertEqual(command[0], "C:/Program Files/compiler.exe")
            self.assertIn("/std:c++20", command)
            self.assertIn("/Foscene.obj", command)
            self.assertIn("/Fescene.exe", command)
            self.assertNotIn("-pthread", command)

    def test_fixture_uses_configured_gnu_frontend(self):
        command = fixture_compile_command(
            "/toolchain/bin/clang++", "GNU", "Clang",
            Path("scene.cpp"), Path("scene"))
        self.assertEqual(command, ["/toolchain/bin/clang++", "-std=c++20",
                                  "-pthread", "scene.cpp", "-o", "scene"])

    def test_pause_joins_preload_before_handoff_and_clears_publication(self):
        self.run_scene_fixture("music_select_scene_pause_fixture.cpp", [
            "void MusicSelectScene::stopPreloadWorker()",
            "void MusicSelectScene::onPause()",
            "void MusicSelectScene::onResume()",
        ], [ROOT / "src/music_select/MusicSelectFolderStatusLoader.cpp"])

    def test_score_revisions_refresh_once_without_a_library_change(self):
        self.run_scene_fixture("music_select_scene_revision_fixture.cpp", [
            "void MusicSelectScene::refreshRepositoryRevisions()",
        ])

    def test_modal_reset_preserves_nondefault_timing_and_analog_configuration(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        signature = "void MusicSelectScene::resetLogicalInput()"
        body = function_body(source, signature)
        fixture = (ROOT / "tests/music_select_scene_input_reset_fixture.cpp").read_text()
        fixture = fixture.replace("MUSIC_SELECT_INPUT_PROCESSOR_HEADER",
                                  (ROOT / "src/music_select/MusicSelectInputProcessor.h").as_posix())
        dependencies = [ROOT / "src/music_select/MusicSelectInputProcessor.cpp"]
        self.compile_and_run(fixture.replace("SCENE_METHODS", signature + body),
                             dependencies)
        layout_only = signature + """{
          if (inputBindingAdapter_) inputBindingAdapter_->reset();
          inputProcessor_ = MusicSelectInputProcessor({
              .layout = musicSelectKeyLayoutForConfig(context.settings.skinMusicSelectInput)});
        }"""
        with self.assertRaises(AssertionError):
            self.compile_and_run(fixture.replace("SCENE_METHODS", layout_only),
                                 dependencies)

    def run_scene_fixture(self, filename, signatures, extra_sources=()):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        methods = "\n".join(
            signature + function_body(source, signature) for signature in signatures
        )
        fixture = (ROOT / "tests" / filename).read_text()
        self.compile_and_run(fixture.replace("SCENE_METHODS", methods)
                             .replace("REPOSITORY_ROOT", ROOT.as_posix()), extra_sources)

    def test_records_callbacks_dispatch_selected_chart_and_saved_result(self):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        callbacks = "\n".join(
            f"callbacks.{name} = [this](const ChartMetaRecord &record, "
            "const ModernChartResultRecord &modern) "
            + function_body(source, f"callbacks.{name} =") + ";"
            for name in ("gbattle", "recallModernChart")
        )
        fixture = (ROOT / "tests/music_select_scene_records_fixture.cpp").read_text()
        self.compile_and_run(fixture.replace("SCENE_CALLBACKS", callbacks))

    def compile_and_run(self, source, extra_sources=()):
        compiler = os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER", "c++")
        frontend = os.environ.get("ASOBMASHOW_TEST_CXX_FRONTEND_VARIANT", "")
        compiler_id = os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER_ID", "")
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / "scene.cpp"
            executable = Path(directory) / (
                "scene.exe" if os.name == "nt" or frontend == "MSVC" or
                compiler_id == "MSVC" else "scene")
            program.write_text(source)
            subprocess.run(
                fixture_compile_command(compiler, frontend, compiler_id,
                                        program, executable, extra_sources),
                cwd=directory, check=True, capture_output=True, text=True,
            )
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True,
                                    timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
