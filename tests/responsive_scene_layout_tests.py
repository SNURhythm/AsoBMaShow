"""Run production layout invalidation with controlled safe-area and view boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract


class ResponsiveSceneLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        fixture = (root / "tests/responsive_scene_layout_fixture.cpp").read_text()
        methods = []
        for scene, layout in (("MusicPlayerScene", "updateLayoutInsets"),
                              ("IrUploadsScene", "updateLayoutInsets"),
                              ("IntroScene", "layoutView")):
            source = (root / f"src/scene/{scene}.cpp").read_text()
            methods.extend(extract(source, f"void {scene}::{name}(")
                           for name in ("update", layout))
        fixture = fixture.replace("SCENE_METHODS", "\n\n".join(methods))
        cls.temp = tempfile.TemporaryDirectory(prefix="asobmashow-safe-area-")
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = Path(cls.temp.name) / "layout.cpp"
        cls.binary = Path(cls.temp.name) / "layout"
        cpp.write_text(fixture)
        subprocess.run(["c++", "-std=c++20", str(cpp), "-o", str(cls.binary)], check=True)

    def test_safe_area_edges_relayout_without_canvas_resize(self):
        for scene in ("music", "ir", "intro", "embedded-music", "embedded-ir"):
            for edge in ("top", "left", "bottom", "right"):
                with self.subTest(scene=scene, edge=edge):
                    subprocess.run([str(self.binary), scene, edge], check=True)


if __name__ == "__main__":
    unittest.main()
