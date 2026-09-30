import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

if __package__:
    from .support.fixture_compiler import FixtureCompiler
else:
    from support.fixture_compiler import FixtureCompiler


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
                    "ASOBMASHOW_TEST_CXX_COMPILER_LAUNCHER": '["/cache path/launcher", "--flag"]',
                }), patch.object(subprocess, "run", return_value=
                    subprocess.CompletedProcess([], 0, "", "")) as run:
                    self.test_selection_is_nonblocking_and_cleanup_respects_worker_ownership()
                    command = run.call_args_list[0].args[0]
                    self.assertEqual(command[:3], ["/cache path/launcher", "--flag",
                                                  "/configured/compiler"])
                    self.assertEqual(len(run.call_args_list), 7)
                    link = run.call_args_list[-2].args[0]
                    self.assertEqual(link[0], "/configured/compiler")
                    self.assertNotIn("/cache path/launcher", link)
                    self.assertIn(standard, command)
                    if frontend == "MSVC":
                        self.assertNotIn("-pthread", command)
                        self.assertTrue(any(flag.startswith("/Fo") for flag in command))
                        self.assertTrue(any(flag.startswith("/Fe") and flag.endswith(".exe")
                                            for flag in link))

    def test_selection_is_nonblocking_and_cleanup_respects_worker_ownership(self):
        source = (ROOT / "src/scene/MainMenuScene.cpp").read_text()
        header = (ROOT / "src/scene/MainMenuScene.h").read_text()
        fixture = (ROOT / "tests/main_menu_preview_lifecycle_fixture.cpp").read_text()
        fixture = fixture.replace("PREVIEW_STATE_FIELDS", "\n".join(
            line for line in header.splitlines()
            if "std::mutex preview" in line
        ))
        fixture = fixture.replace("SELECTION_CALLBACK", callback_body(
            source, "recyclerView->onSelected = [this, &context]"
        ))
        compiler = FixtureCompiler.from_environment()
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / "preview.cpp"
            executable = Path(directory) / ("preview" + compiler.executable_suffix)
            program.write_text('#include "i18n/Localization.h"\n' + fixture)
            sources = [str(program), str(ROOT / "src/i18n/Localization.cpp"),
                str(ROOT / "src/scene/ChartPreloadWorker.cpp"),
                str(ROOT / "src/scene/MainMenuPreviewController.cpp"),
                str(ROOT / "src/path.cpp")]
            compiler.build(sources, executable, directory, standard=23,
                           includes=[ROOT / "include", ROOT / "src"])
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
