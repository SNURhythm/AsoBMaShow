#!/usr/bin/env python3
"""Exercise object reuse without changing fixture compilation or link ownership."""

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "cmake/SharedTestSources.cmake"


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake/Ninja required")
class SharedTestSourcesTests(unittest.TestCase):
    def run_command(self, *args):
        result = subprocess.run(args, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_reuse_preserves_definitions_assertions_and_source_subsets(self):
        # Sharing the variant object or injecting extra.cpp into b would either
        # change its output or fail to link (extra.cpp needs a's fixture symbol).
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "common.cpp").write_text(
                "#ifdef NDEBUG\n#error Assertions must remain enabled\n#endif\n"
                "int common() { return VALUE; }\n")
            (root / "extra.cpp").write_text(
                "extern int fixture(); int extra() { return fixture(); }\n")
            (root / "a.cpp").write_text(
                "#include <cstdio>\nint common(); int extra();\n"
                "int fixture() { return 5; }\n"
                "int main() { printf(\"%d\", common()+extra()); }\n")
            (root / "b.cpp").write_text(
                "#include <cstdio>\nint common();\n"
                "int main() { printf(\"%d\", common()); }\n")
            (root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.22)
project(share_fixture LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include("{MODULE.as_posix()}")
add_library(requirements INTERFACE)
target_compile_features(requirements INTERFACE cxx_std_17)
add_executable(a a.cpp common.cpp extra.cpp)
add_executable(b b.cpp common.cpp)
add_executable(variant b.cpp common.cpp)
foreach(t a b variant)
    target_link_libraries(${{t}} PRIVATE $<TARGET_NAME_IF_EXISTS:requirements>)
    if(MSVC)
        target_compile_options(${{t}} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${{t}} PRIVATE -UNDEBUG)
    endif()
endforeach()
target_compile_definitions(a PRIVATE VALUE=7)
target_compile_definitions(b PRIVATE VALUE=7)
target_compile_definitions(variant PRIVATE VALUE=11)
asobmashow_share_test_sources(fixture
    SOURCES common.cpp extra.cpp TARGETS a b variant)
""")
            for configuration in ("Debug", "Release", "PIC"):
                with self.subTest(configuration=configuration):
                    build = root / configuration
                    options = (["-DCMAKE_POSITION_INDEPENDENT_CODE=ON"]
                               if configuration == "PIC" else [])
                    self.run_command("cmake", "-S", str(root), "-B", str(build),
                                     "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" +
                                     ("Debug" if configuration == "PIC" else configuration), *options)
                    self.run_command("cmake", "--build", str(build))
                    for executable, expected in (("a", "12"), ("b", "7"), ("variant", "11")):
                        self.assertEqual(self.run_command(str(build / executable)), expected)
                    commands = json.loads((build / "compile_commands.json").read_text())
                    common = [e for e in commands if Path(e["file"]).name == "common.cpp"]
                    self.assertEqual(len(common), 3 if configuration == "PIC" else 2,
                                     "reuse must preserve executable PIE compilation")
                    self.assertIn("no work to do", self.run_command("cmake", "--build", str(build)))


if __name__ == "__main__":
    unittest.main()
