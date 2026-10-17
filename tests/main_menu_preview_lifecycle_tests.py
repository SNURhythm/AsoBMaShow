import subprocess
import tempfile
import unittest
from pathlib import Path

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
    def test_play_reuses_preview_resources_for_lane_modifiers(self):
        source = (ROOT / "src/scene/MainMenuScene.cpp").read_text()
        fixture = (ROOT / "tests/main_menu_preview_start_fixture.cpp").read_text()
        fixture = fixture.replace("START_METHOD", callback_body(
            source, "void MainMenuScene::startChartDirect(const ChartMetaRecord &record)"))
        compiler = FixtureCompiler.from_environment(shared_headers=True)
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / "start.cpp"
            executable = Path(directory) / ("start" + compiler.executable_suffix)
            program.write_text(fixture)
            compiler.build([str(program)], executable, directory, standard=23,
                           includes=[ROOT / "src"])
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_progressive_preview_prepares_and_retires_visuals(self):
        source = (ROOT / "src/scene/MainMenuScene.cpp").read_text()
        fixture = (ROOT / "tests/main_menu_preview_visuals_fixture.cpp").read_text()
        fixture = fixture.replace("PREVIEW_PROCESS", callback_body(
            source, "[this](const ChartMetaRecord &request, std::atomic_bool &cancelled)"))
        stop = source.index("}, [this] { stopAndClearSelectedChart(); }, kPreviewDebounceDelay,")
        fixture = fixture.replace("PREVIEW_STOP", callback_body(source[stop:], "[this] {\n"))
        signature = "void MainMenuScene::updatePreviewVisuals()"
        self.assertTrue(signature in source, "preview must drive BGA from consumed audio frames")
        fixture = fixture.replace("SYNC_METHOD", "void updatePreviewVisuals() " +
                                  callback_body(source, signature))

        compiler = FixtureCompiler.from_environment(shared_headers=True)
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / "visuals.cpp"
            executable = Path(directory) / ("visuals" + compiler.executable_suffix)
            program.write_text(fixture)
            compiler.build([str(program)], executable, directory, standard=23,
                           includes=[ROOT / "src"])
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_selection_is_nonblocking_and_cleanup_respects_worker_ownership(self):
        source = (ROOT / "src/scene/MainMenuScene.cpp").read_text()
        header = (ROOT / "src/scene/MainMenuScene.h").read_text()
        fixture = (ROOT / "tests/main_menu_preview_lifecycle_fixture.cpp").read_text()
        fixture = fixture.replace("PREVIEW_STATE_FIELDS", "\n".join(
            line for line in header.splitlines()
            if "std::mutex preview" in line
        ))
        fixture = fixture.replace("SELECTION_CALLBACK", callback_body(
            source, "const auto selectChart = [this, &context]"
        ))
        wiring_start = source.index("  recyclerView->onSelected = [selectChart]")
        wiring_end = source.index("  recyclerView->onUnselected", wiring_start)
        fixture = fixture.replace("SELECTION_WIRING", source[wiring_start:wiring_end])
        compiler = FixtureCompiler.from_environment(shared_headers=True)
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
