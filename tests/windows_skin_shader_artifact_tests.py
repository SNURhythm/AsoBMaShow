import os
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

    def test_configured_git_executable_is_used_for_worktree_inspection(self):
        git_executable = shutil.which("git")
        self.assertIsNotNone(git_executable, "Git is required for this contract")
        with tempfile.TemporaryDirectory() as temporary_directory:
            isolated_root = Path(temporary_directory)
            for relative in (
                Path("shader_src/vs_skin_quad.sc"),
                Path("shader_src/fs_skin_quad.sc"),
                Path("shaders/dx11/vs_skin_quad.bin"),
                Path("shaders/dx11/fs_skin_quad.bin"),
            ):
                destination = isolated_root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, destination)
            subprocess.run(
                [git_executable, "init", "-q"], cwd=isolated_root, check=True
            )
            environment = os.environ.copy()
            environment["ASOBMASHOW_GIT_EXECUTABLE"] = git_executable
            environment["PATH"] = ""

            result = subprocess.run(
                [
                    sys.executable,
                    str(VERIFY_SHADER_OUTPUTS),
                    "--root",
                    str(isolated_root),
                    "--shader",
                    "skin_quad",
                    "--require-backends",
                    "dx11",
                ],
                env=environment,
                text=True,
                capture_output=True,
                check=False,
            )

        self.assertEqual(0, result.returncode, result.stderr)

    def test_unavailable_configured_git_executable_fails_clearly(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            isolated_root = Path(temporary_directory)
            (isolated_root / ".git").mkdir()
            for relative in (
                Path("shader_src/vs_skin_quad.sc"),
                Path("shader_src/fs_skin_quad.sc"),
                Path("shaders/dx11/vs_skin_quad.bin"),
                Path("shaders/dx11/fs_skin_quad.bin"),
            ):
                destination = isolated_root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, destination)
            unavailable = isolated_root / "missing" / "git.exe"
            environment = os.environ.copy()
            environment["ASOBMASHOW_GIT_EXECUTABLE"] = str(unavailable)

            result = subprocess.run(
                [
                    sys.executable,
                    str(VERIFY_SHADER_OUTPUTS),
                    "--root",
                    str(isolated_root),
                    "--shader",
                    "skin_quad",
                    "--require-backends",
                    "dx11",
                ],
                env=environment,
                text=True,
                capture_output=True,
                check=False,
            )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("configured Git executable is unavailable", result.stderr)


if __name__ == "__main__":
    unittest.main()
