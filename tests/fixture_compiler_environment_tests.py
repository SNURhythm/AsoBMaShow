"""Configure CTest and run real fixture compilation through launcher expressions."""

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class FixtureCompilerEnvironmentTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"),
                         "CMake and Ninja are required")
    def test_launcher_expressions_reach_fixture_compilation_per_configuration(self):
        source = (ROOT / "CMakeLists.txt").read_text()
        start = source.index("    # Runtime C++ fixtures should share")
        end = source.index("\n    add_test(NAME music_select_error_flow_contract", start)
        export = source[start:end]
        with tempfile.TemporaryDirectory(prefix="fixture launcher ") as directory:
            root = Path(directory)
            launcher = root / "record launcher.py"
            launcher.write_text(
                "import json, os, subprocess, sys\n"
                "boundary = sys.argv.index('--')\n"
                "with open(os.environ['LAUNCHER_LOG'], 'w') as output:\n"
                "    json.dump(sys.argv[1:boundary], output)\n"
                "sys.exit(subprocess.call(sys.argv[boundary + 1:]))\n")
            runner = root / "compile.py"
            runner.write_text(
                "import json, os, subprocess, sys\n"
                "from pathlib import Path\n"
                f"sys.path.insert(0, {str(ROOT)!r})\n"
                "from tests.support.fixture_compiler import FixtureCompiler\n"
                "expected = json.loads(Path(os.environ['EXPECTED_LAUNCHER']).read_text())\n"
                "compiler = FixtureCompiler.from_environment()\n"
                "assert list(compiler.launcher) == expected, (compiler.launcher, expected)\n"
                "work = Path(os.environ['FIXTURE_WORK'])\n"
                "work.mkdir(exist_ok=True)\n"
                "source = work / 'main.cpp'\n"
                "source.write_text('int main() { return 0; }')\n"
                "executable = work / ('main' + compiler.executable_suffix)\n"
                "compiler.build([source], executable, work)\n"
                "subprocess.run([str(executable)], check=True)\n"
                "log = Path(os.environ['LAUNCHER_LOG'])\n"
                "if expected:\n"
                "    assert json.loads(log.read_text()) == expected[2:-1]\n"
                "else:\n"
                "    assert not log.exists()\n")
            prefix = [sys.executable, str(launcher)]
            command = ";".join(prefix)
            cases = [
                ("conditional", f"$<$<CONFIG:Debug>:{command};debug;-->", "",
                 {"Debug": prefix + ["debug", "--"], "Release": []}),
                ("branch-list", f"$<IF:$<CONFIG:Debug>,{command};debug;--,{command};release;-->", "",
                 {config: prefix + [config.lower(), "--"] for config in ("Debug", "Release")}),
                ("empty-argument", f"$<IF:$<CONFIG:Debug>,{command};;debug;--,{command};;release;-->", "",
                 {config: prefix + ["", config.lower(), "--"] for config in ("Debug", "Release")}),
                ("literal-arguments", f"{command};;literal;--", "",
                 {config: prefix + ["", "literal", "--"] for config in ("Debug", "Release")}),
                ("property-list", "$<TARGET_PROPERTY:launcher,COMMAND>",
                 f"add_library(launcher INTERFACE)\nset_property(TARGET launcher PROPERTY COMMAND [==[{command};space value;semi\\;colon;quote\"value;back\\slash;--]==])\n",
                 {config: prefix + ["space value", "semi;colon", 'quote"value', "back\\slash", "--"]
                  for config in ("Debug", "Release")}),
            ]
            for name, expression, setup, expected in cases:
                with self.subTest(expression=name):
                    project = root / name
                    project.mkdir()
                    for config, arguments in expected.items():
                        (project / f"expected-{config}.json").write_text(json.dumps(arguments))
                    (project / "CMakeLists.txt").write_text(
                        "cmake_minimum_required(VERSION 3.22)\nproject(LauncherProbe CXX)\n"
                        "enable_testing()\n"
                        f"set(Python3_EXECUTABLE [==[{sys.executable}]==])\n"
                        f"set(CMAKE_CXX_COMPILER_LAUNCHER [==[{expression}]==])\n"
                        + setup + export + "\n"
                        f'add_test(NAME probe COMMAND "${{Python3_EXECUTABLE}}" "{runner.as_posix()}")\n'
                        'set_tests_properties(probe PROPERTIES ENVIRONMENT '
                        '"${ASOBMASHOW_TEST_CXX_ENVIRONMENT};'
                        'EXPECTED_LAUNCHER=${CMAKE_CURRENT_SOURCE_DIR}/expected-$<CONFIG>.json;'
                        'FIXTURE_WORK=${CMAKE_CURRENT_BINARY_DIR}/work-$<CONFIG>;'
                        'LAUNCHER_LOG=${CMAKE_CURRENT_BINARY_DIR}/launcher-$<CONFIG>.json")\n')
                    build = project / "build"
                    configured = subprocess.run(
                        ["cmake", "-S", str(project), "-B", str(build), "-G", "Ninja Multi-Config"],
                        capture_output=True, text=True)
                    self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
                    for config in expected:
                        tested = subprocess.run(
                            ["ctest", "--test-dir", str(build), "-C", config, "--output-on-failure"],
                            capture_output=True, text=True)
                        self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == "__main__":
    unittest.main()
