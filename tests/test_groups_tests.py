#!/usr/bin/env python3
"""Focused builds must include fixture prerequisites without building other areas."""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "cmake/TestGroups.cmake"


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja required")
class TestGroupsTests(unittest.TestCase):
    def run_command(self, *args):
        result = subprocess.run(args, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_focused_build_includes_transitive_setup_cleanup_and_wrapped_targets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "check.cmake").write_text(
                'if(NOT EXISTS "${READY}")\nmessage(FATAL_ERROR "Missing ${READY}")\nendif()\n')
            (root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.22)
project(test_groups NONE)
enable_testing()
include("{MODULE.as_posix()}")
foreach(name replay_native prepare cleanup second_prepare unrelated)
    add_custom_target(${{name}}
        COMMAND ${{CMAKE_COMMAND}} -E touch ${{CMAKE_BINARY_DIR}}/${{name}}.ready)
endforeach()
foreach(name replay_case prepare cleanup second_prepare unrelated)
    set(native ${{name}})
    if(name STREQUAL "replay_case")
        set(native replay_native)
    endif()
    add_test(NAME ${{name}} COMMAND ${{CMAKE_COMMAND}}
        -DREADY=${{CMAKE_BINARY_DIR}}/${{native}}.ready -P ${{CMAKE_SOURCE_DIR}}/check.cmake)
endforeach()
set_tests_properties(replay_case PROPERTIES LABELS "integration" FIXTURES_REQUIRED first)
set_tests_properties(prepare PROPERTIES FIXTURES_SETUP first FIXTURES_REQUIRED second)
set_tests_properties(cleanup PROPERTIES FIXTURES_CLEANUP first)
set_tests_properties(second_prepare PROPERTIES FIXTURES_SETUP second)
asobmashow_test_build_targets(replay_case replay_native)
asobmashow_label_tests(area_replay replay_case)
asobmashow_label_tests(area_core prepare cleanup second_prepare)
asobmashow_label_tests(area_other unrelated platform_only_test_not_on_this_host)
asobmashow_finalize_test_groups()
""")
            build = root / "build"
            self.run_command("cmake", "-S", str(root), "-B", str(build), "-G", "Ninja")
            self.run_command("cmake", "--build", str(build), "--target", "tests_replay", "-j", "2")
            self.assertFalse((build / "unrelated.ready").exists())
            self.run_command("ctest", "--test-dir", str(build), "-L", "^area_replay$",
                             "--output-on-failure", "--no-tests=error")
            catalog = json.loads(self.run_command(
                "ctest", "--test-dir", str(build), "--show-only=json-v1", "-L", "^area_replay$"))
            self.assertEqual({test["name"] for test in catalog["tests"]},
                             {"replay_case", "prepare", "cleanup", "second_prepare"})
            replay = next(test for test in catalog["tests"] if test["name"] == "replay_case")
            labels = next(p["value"] for p in replay["properties"] if p["name"] == "LABELS")
            self.assertEqual(labels, ["area_replay", "integration"])

    def test_unclassified_test_cannot_silently_disappear_from_focused_runs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.22)
project(test_groups NONE)
enable_testing()
include("{MODULE.as_posix()}")
add_test(NAME new_test COMMAND ${{CMAKE_COMMAND}} -E true)
asobmashow_finalize_test_groups()
""")
            result = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")],
                                    text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unclassified test: new_test", result.stderr)


if __name__ == "__main__":
    unittest.main()
