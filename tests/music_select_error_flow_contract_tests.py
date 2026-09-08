#!/usr/bin/env python3
import unittest
import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def fixture_compile_command(compiler, frontend, compiler_id, source, executable):
    if frontend == "MSVC" or compiler_id == "MSVC":
        return [compiler, "/nologo", "/std:c++20", "/EHsc", str(source),
                f"/Fo{source.with_suffix('.obj')}", f"/Fe{executable}"]
    return [compiler, "-std=c++20", "-pthread", str(source), "-o", str(executable)]


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

    def run_scene_fixture(self, filename, signatures):
        source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
        methods = "\n".join(
            signature + function_body(source, signature) for signature in signatures
        )
        fixture = (ROOT / "tests" / filename).read_text()
        self.compile_and_run(fixture.replace("SCENE_METHODS", methods))

    def compile_and_run(self, source):
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
                                        program, executable),
                cwd=directory, check=True, capture_output=True, text=True,
            )
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
