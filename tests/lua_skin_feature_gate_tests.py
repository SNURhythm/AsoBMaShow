#!/usr/bin/env python3

import os
import pathlib
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FEATURE_HEADER = ROOT / "src/skin/LuaGameplaySkinFeature.h"
ENABLED_SOURCE = ROOT / "src/skin/LuaGameplaySkinFeatureEnabled.cpp"
UNAVAILABLE_SOURCE = ROOT / "src/skin/LuaGameplaySkinFeatureUnavailable.cpp"
ROOT_CMAKE = ROOT / "CMakeLists.txt"
SKIN_CMAKE = ROOT / "src/skin/CMakeLists.txt"
RUNTIME_SOURCE = ROOT / "src/skin/beatoraja/LuaSkinRuntime.cpp"
HOST_SOURCE = ROOT / "src/skin/beatoraja/LuaSkinHostModules.cpp"
DECODER_SOURCE = ROOT / "src/skin/beatoraja/LuaSkinTableDecoder.cpp"
VALIDATOR_SOURCE = ROOT / "src/skin/beatoraja/SkinModelValidator.cpp"
GAUGE_NODE_EXPANSION_SOURCE = (
    ROOT / "src/skin/beatoraja/SkinGaugeNodeExpansion.cpp"
)


class LuaSkinFeatureGateTests(unittest.TestCase):
    def compile_and_run_query(self, definitions=()):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        self.assertIsNotNone(compiler, "a C++ compiler is required")

        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = pathlib.Path(temporary_directory)
            main = temporary / "main.cpp"
            executable = temporary / "feature-query"
            main.write_text(
                textwrap.dedent(
                    """\
                    #include "skin/LuaGameplaySkinFeature.h"
                    #include <iostream>

                    int main() {
                      std::cout << skin::luaGameplaySkinsAvailable();
                    }
                    """
                ),
                encoding="utf-8",
            )
            command = [
                compiler,
                "-std=c++23",
                *definitions,
                "-I",
                str(ROOT / "src"),
                str(main),
                str(ENABLED_SOURCE),
                str(UNAVAILABLE_SOURCE),
                "-o",
                str(executable),
            ]
            subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
            return subprocess.run(
                [str(executable)],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            ).stdout

    def test_desktop_and_ios_style_compiles_enable_the_feature_by_default(self):
        self.assertEqual("1", self.compile_and_run_query())

    def test_android_style_compile_disables_the_feature_by_default(self):
        self.assertEqual("0", self.compile_and_run_query(("-D__ANDROID__=1",)))

    def test_build_definition_overrides_the_platform_default(self):
        self.assertEqual(
            "0",
            self.compile_and_run_query(
                ("-DASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=0",)
            ),
        )
        self.assertEqual(
            "1",
            self.compile_and_run_query(
                (
                    "-D__ANDROID__=1",
                    "-DASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1",
                )
            ),
        )

    def test_cmake_defaults_android_off_and_other_platforms_on(self):
        source = ROOT_CMAKE.read_text(encoding="utf-8")
        android_default = source.index(
            "set(ASOBMASHOW_LUA_GAMEPLAY_SKINS_DEFAULT OFF)"
        )
        other_default = source.index(
            "set(ASOBMASHOW_LUA_GAMEPLAY_SKINS_DEFAULT ON)"
        )
        option = source.index("option(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS")
        definition = source.index("ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=$<BOOL:")
        self.assertLess(android_default, option)
        self.assertLess(other_default, option)
        self.assertLess(option, definition)

    def test_cmake_selects_exactly_one_complementary_translation_unit(self):
        source = SKIN_CMAKE.read_text(encoding="utf-8")
        conditional_start = source.index(
            "if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)"
        )
        alternative = source.index("else()", conditional_start)
        conditional_end = source.index("endif()", alternative)
        enabled = source[conditional_start:alternative]
        unavailable = source[alternative:conditional_end]
        self.assertIn("LuaGameplaySkinFeatureEnabled.cpp", enabled)
        self.assertNotIn("LuaGameplaySkinFeatureUnavailable.cpp", enabled)
        self.assertIn("LuaGameplaySkinFeatureUnavailable.cpp", unavailable)
        self.assertNotIn("LuaGameplaySkinFeatureEnabled.cpp", unavailable)

    def test_enabled_runtime_sources_are_selected_only_behind_the_gate(self):
        source = SKIN_CMAKE.read_text(encoding="utf-8")
        enabled_block = source[
            source.index("if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)") :
            source.index("else()", source.index("if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)"))
        ]
        self.assertIn("beatoraja/LuaSkinRuntime.cpp", enabled_block)
        self.assertIn("beatoraja/LuaSkinHostModules.cpp", enabled_block)
        self.assertIn("beatoraja/LuaSkinTableDecoder.cpp", enabled_block)
        self.assertIn("beatoraja/SkinModelValidator.cpp", enabled_block)
        self.assertIn("beatoraja/SkinGaugeNodeExpansion.cpp", enabled_block)

    def test_xcode_discovered_runtime_implementations_have_source_guards(self):
        for path in (
            RUNTIME_SOURCE,
            HOST_SOURCE,
            DECODER_SOURCE,
            VALIDATOR_SOURCE,
            GAUGE_NODE_EXPANSION_SOURCE,
        ):
            source = path.read_text(encoding="utf-8")
            self.assertIn('#include "../LuaGameplaySkinFeature.h"', source)
            self.assertIn("#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS", source)
            self.assertIn("#endif", source)

    def test_disabled_runtime_sources_compile_without_lua_headers_or_libraries(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        self.assertIsNotNone(compiler, "a C++ compiler is required")
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = pathlib.Path(temporary_directory)
            for source in (
                RUNTIME_SOURCE,
                HOST_SOURCE,
                DECODER_SOURCE,
                VALIDATOR_SOURCE,
                GAUGE_NODE_EXPANSION_SOURCE,
            ):
                subprocess.run(
                    [
                        compiler,
                        "-std=c++23",
                        "-D__ANDROID__=1",
                        "-DASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=0",
                        "-I",
                        str(ROOT / "src"),
                        "-c",
                        str(source),
                        "-o",
                        str(temporary / (source.stem + ".o")),
                    ],
                    cwd=ROOT,
                    check=True,
                    capture_output=True,
                    text=True,
                )


if __name__ == "__main__":
    unittest.main()
