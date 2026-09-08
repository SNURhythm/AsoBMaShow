import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def callback_body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 0
    for index in range(start, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError(f"unterminated callback: {signature}")


class MainMenuPreviewLifecycleTests(unittest.TestCase):
    def test_fixture_uses_configured_compiler_frontend(self):
        for frontend, compiler_id, standard in (
            ("GNU", "Clang", "-std=c++23"),
            ("MSVC", "MSVC", "/std:c++latest"),
            ("MSVC", "Clang", "/std:c++latest"),
        ):
            with self.subTest(frontend=frontend, compiler_id=compiler_id):
                with patch.dict(os.environ, {
                    "ASOBMASHOW_TEST_CXX_COMPILER": "/configured/compiler",
                    "ASOBMASHOW_TEST_CXX_FRONTEND_VARIANT": frontend,
                    "ASOBMASHOW_TEST_CXX_COMPILER_ID": compiler_id,
                }), patch.object(subprocess, "run", return_value=
                    subprocess.CompletedProcess([], 0, "", "")) as run:
                    self.test_selection_is_nonblocking_and_cleanup_respects_worker_ownership()
                    command = run.call_args_list[0].args[0]
                    self.assertEqual(command[0], "/configured/compiler")
                    self.assertIn(standard, command)
                    if frontend == "MSVC":
                        self.assertNotIn("-pthread", command)
                        self.assertTrue(any(flag.startswith("/Fo") for flag in command))
                        self.assertTrue(any(flag.startswith("/Fe") and flag.endswith(".exe")
                                            for flag in command))

    def test_selection_is_nonblocking_and_cleanup_respects_worker_ownership(self):
        source = (ROOT / "src/scene/MainMenuScene.cpp").read_text()
        header = (ROOT / "src/scene/MainMenuScene.h").read_text()
        fixture = (ROOT / "tests/main_menu_preview_lifecycle_fixture.cpp").read_text()
        fixture = fixture.replace("PREVIEW_STATE_FIELDS", "\n".join(
            line for line in header.splitlines()
            if "pendingStopAndClearSelectedChartAfterPreview =" in line
            or "std::mutex preview" in line
        ))
        fixture = fixture.replace("SELECTION_CALLBACK", callback_body(
            source, "recyclerView->onSelected = [this, &context]"
        ))
        fixture = fixture.replace("IDLE_CALLBACK", callback_body(
            source, "previewWorker_->setOnIdle([this]()"
        ))
        compiler = os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER", "c++")
        frontend = os.environ.get("ASOBMASHOW_TEST_CXX_FRONTEND_VARIANT", "")
        compiler_id = os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER_ID", "")
        msvc = frontend == "MSVC" or compiler_id == "MSVC"
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / "preview.cpp"
            executable = Path(directory) / ("preview.exe" if os.name == "nt" or msvc
                                             else "preview")
            program.write_text(fixture)
            sources = [str(program),
                str(ROOT / "src/scene/ChartPreloadWorker.cpp"),
                str(ROOT / "src/path.cpp")]
            if msvc:
                command = [compiler, "/nologo", "/std:c++latest", "/EHsc",
                           f"/I{ROOT / 'include'}", f"/I{ROOT / 'src'}", *sources,
                           f"/Fo{directory}{os.sep}", f"/Fe{executable}"]
            else:
                command = [compiler, "-std=c++23", "-pthread",
                           "-I", str(ROOT / "include"), "-I", str(ROOT / "src"),
                           *sources, "-o", str(executable)]
            build = subprocess.run(command, cwd=directory,
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(executable)], capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
