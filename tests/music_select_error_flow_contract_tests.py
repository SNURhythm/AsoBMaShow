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
        ])

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

    def run_scene_fixture(self, filename, signatures):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        methods = "\n".join(
            signature + function_body(source, signature) for signature in signatures
        )
        fixture = (ROOT / "tests" / filename).read_text()
        self.compile_and_run(fixture.replace("SCENE_METHODS", methods))

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
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
