#!/usr/bin/env python3
"""Integration coverage for the incremental CMake build-identity refresh."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_IDENTITY_MODULE = ROOT / "cmake" / "BuildIdentity.cmake"


class BuildIdentityRefreshTests(unittest.TestCase):
    def run_command(
        self, *command: str, cwd: Path
    ) -> subprocess.CompletedProcess[str]:
        completed = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if completed.returncode != 0:
            self.fail(
                "command failed: "
                + " ".join(command)
                + "\nstdout:\n"
                + completed.stdout
                + "\nstderr:\n"
                + completed.stderr
            )
        return completed

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("git"),
                         "CMake and Git are required")
    def test_incremental_build_refreshes_dirty_source_identity(self) -> None:
        """A source edit makes the next build embed dirty state without a clean rebuild."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "fixture"
            fixture.mkdir()
            module_path = BUILD_IDENTITY_MODULE.as_posix()
            (fixture / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.22)\n"
                "project(build_identity_fixture LANGUAGES CXX)\n"
                f'include("{module_path}")\n'
                "add_executable(identity main.cpp)\n"
                "asobmashow_configure_build_identity_refresh(identity "
                '"${CMAKE_SOURCE_DIR}")\n',
                encoding="utf-8",
            )
            (fixture / "main.cpp").write_text(
                '#include "BuildIdentityConfig.h"\n'
                "#include <iostream>\n"
                "int main() { std::cout << ASOBMASHOW_SOURCE_CLEAN; }\n",
                encoding="utf-8",
            )
            (fixture / "tracked.txt").write_text("initial\n", encoding="utf-8")
            self.run_command("git", "init", "-q", cwd=fixture)
            self.run_command(
                "git", "config", "user.email", "fixture@example.invalid", cwd=fixture
            )
            self.run_command(
                "git", "config", "user.name", "Build Identity Fixture", cwd=fixture
            )
            self.run_command("git", "add", ".", cwd=fixture)
            self.run_command("git", "commit", "-qm", "initial", cwd=fixture)

            build = Path(temporary) / "build"
            self.run_command(
                "cmake",
                "-S",
                str(fixture),
                "-B",
                str(build),
                "-DCMAKE_BUILD_TYPE=Debug",
                cwd=fixture,
            )
            self.run_command("cmake", "--build", str(build), "-j", "2", cwd=fixture)
            first = self.run_command(str(build / "identity"), cwd=fixture)
            self.assertEqual("1", first.stdout)

            (fixture / "tracked.txt").write_text("modified\n", encoding="utf-8")
            self.run_command("cmake", "--build", str(build), "-j", "2", cwd=fixture)
            second = self.run_command(str(build / "identity"), cwd=fixture)
            self.assertEqual("0", second.stdout)


if __name__ == "__main__":
    unittest.main()
