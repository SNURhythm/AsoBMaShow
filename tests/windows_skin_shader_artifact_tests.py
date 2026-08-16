import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERIFY_SHADER_OUTPUTS = ROOT / "scripts/verify_skin_shader_outputs.py"


class WindowsSkinShaderArtifactTests(unittest.TestCase):
    def test_dx11_gameplay_skin_shaders_are_packaged(self):
        for shader in ("skin_quad", "skin_yuvrgb"):
            with self.subTest(shader=shader):
                with tempfile.TemporaryDirectory() as temporary_directory:
                    isolated_root = Path(temporary_directory)
                    for relative in (
                        Path("shader_src") / f"vs_{shader}.sc",
                        Path("shader_src") / f"fs_{shader}.sc",
                        Path("shaders") / "dx11" / f"vs_{shader}.bin",
                        Path("shaders") / "dx11" / f"fs_{shader}.bin",
                    ):
                        destination = isolated_root / relative
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        if (ROOT / relative).is_file():
                            shutil.copy2(ROOT / relative, destination)

                    result = subprocess.run(
                        [
                            sys.executable,
                            str(VERIFY_SHADER_OUTPUTS),
                            "--root",
                            str(isolated_root),
                            "--shader",
                            shader,
                            "--require-backends",
                            "dx11",
                        ],
                        text=True,
                        capture_output=True,
                        check=False,
                    )

                self.assertEqual(0, result.returncode, result.stderr)


if __name__ == "__main__":
    unittest.main()
