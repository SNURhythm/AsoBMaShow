"""Exercise production goal editor and deferred profile guards with small fakes."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler


class ChartGoalsSceneTests(unittest.TestCase):
    def test_edits_failures_deletion_and_profile_handoffs(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        methods = "\n".join(extract(scene, signature) for signature in (
            "bool ChartGoalsScene::checkProfile(", "void ChartGoalsScene::later(",
            "void ChartGoalsScene::saveGoal(", "void ChartGoalsScene::removeGoal("))
        fixture = (root / "tests/chart_goals_scene_fixture.cpp").read_text()
        fixture = fixture.replace("PRODUCTION_METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="asobmashow-goals-scene-") as directory:
            source = Path(directory) / "goals.cpp"
            binary = Path(directory) / "goals"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [source, root / "src/goals/ChartGoal.cpp"], binary, source.parent,
                includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_navigation_is_deferred_and_practice_returns_once(self):
        root = Path(__file__).resolve().parents[1]
        scene = (root / "src/scene/ChartGoalsScene.cpp").read_text()
        base = (root / "src/scene/Scene.h").read_text()
        fixture = (root / "tests/chart_goals_navigation_fixture.cpp").read_text()
        fixture = fixture.replace("NAVIGATE", extract(scene, "void ChartGoalsScene::navigate("))
        fixture = fixture.replace("DEFER_METHODS", "\n".join(extract(base, signature) for signature in (
            "void defer(", "void handleDeferred()")))
        with tempfile.TemporaryDirectory(prefix="asobmashow-goals-nav-") as directory:
            source = Path(directory) / "navigation.cpp"
            binary = Path(directory) / "navigation"
            source.write_text(fixture)
            FixtureCompiler.from_environment(shared_headers=True).build(
                [source], binary, source.parent, includes=(root / "src",))
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
