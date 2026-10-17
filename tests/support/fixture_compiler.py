"""Compile temporary behavioral fixtures with the configured CMake toolchain."""

import json
import os
import subprocess
import sys
from dataclasses import dataclass, replace
from functools import lru_cache
from pathlib import Path


@lru_cache(maxsize=None)
def _generated_launcher_json(path, cmake):
    # Generator expressions have already been evaluated in this per-config
    # file. Use CMake's list parser, then serialize argv without shell quoting.
    result = subprocess.run(
        [cmake, f"-DLAUNCHER_FILE={path}", f"-DPYTHON_EXECUTABLE={sys.executable}",
         "-P", str(Path(__file__).with_name("fixture_compiler_launcher.cmake"))],
        check=True, capture_output=True, text=True)
    return result.stdout


@dataclass(frozen=True)
class FixtureCompiler:
    compiler: str
    frontend: str = ""
    compiler_id: str = ""
    launcher: tuple = ()

    @classmethod
    def from_environment(cls):
        encoded = os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER_LAUNCHER")
        launcher_file = os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER_LAUNCHER_FILE")
        if not encoded and launcher_file:
            encoded = _generated_launcher_json(
                launcher_file, os.environ.get("ASOBMASHOW_TEST_CMAKE_COMMAND", "cmake"))
        launcher = json.loads(encoded or "[]")
        if not isinstance(launcher, list) or any(not isinstance(arg, str) for arg in launcher):
            raise ValueError("ASOBMASHOW_TEST_CXX_COMPILER_LAUNCHER must be a JSON array of strings")
        # A conditional CMake launcher can evaluate to one empty list element.
        # Preserve empty arguments to an enabled launcher; only this disables it.
        if launcher == [""]:
            launcher = []
        return cls(os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER", "c++"),
                   os.environ.get("ASOBMASHOW_TEST_CXX_FRONTEND_VARIANT", ""),
                   os.environ.get("ASOBMASHOW_TEST_CXX_COMPILER_ID", ""), tuple(launcher))

    @property
    def msvc(self):
        return self.frontend == "MSVC" or self.compiler_id == "MSVC"

    @property
    def executable_suffix(self):
        return ".exe" if os.name == "nt" or self.msvc else ""

    def compile_command(self, source, output, standard=20, includes=()):
        command = [*self.launcher, self.compiler]
        if self.msvc:
            version = "c++latest" if standard > 20 else f"c++{standard}"
            return [*command, "/nologo", f"/std:{version}", "/EHsc", "/utf-8",
                    *(f"/I{path}" for path in includes), str(source), "/c", f"/Fo{output}"]
        return [*command, f"-std=c++{standard}", "-pthread",
                *(flag for path in includes for flag in ("-I", str(path))),
                str(source), "-c", "-o", str(output)]

    def link_command(self, objects, executable):
        # Launchers such as ccache only cache individual compilation commands.
        if self.msvc:
            return [self.compiler, "/nologo", *map(str, objects), f"/Fe{executable}"]
        return [self.compiler, "-pthread", *map(str, objects), "-o", str(executable)]

    def build(self, sources, executable, directory, standard=20, includes=(), objects=(),
              compile_options=(), link_options=()):
        objects = list(objects)
        directory = Path(directory)
        for index, source in enumerate(sources):
            source = Path(source)
            # Stable relative input names let compiler caches reuse generated TUs
            # across temporary directories without keeping any fixture artifacts.
            if source.is_absolute() and source.parent == directory:
                source = Path(source.name)
            output = Path(f"fixture-{index}.obj")
            subprocess.run([*self.compile_command(source, output, standard, includes),
                            *compile_options],
                           cwd=directory, check=True, capture_output=True, text=True)
            objects.append(output)
        subprocess.run([*self.link_command(objects, Path(executable).name), *link_options],
                       cwd=directory, check=True, capture_output=True, text=True)


@lru_cache(maxsize=1)
def _macos_toolchain():
    def resolve(option):
        return subprocess.check_output(
            ["xcrun", "--sdk", "macosx", *option], text=True).strip()
    return resolve(["--find", "clang++"]), resolve(["--show-sdk-path"])


def build_foundation_fixture(source, executable, includes=()):
    # Resolve xcrun first so the cache sees the actual compiler and SDK identity.
    clang, sdk = _macos_toolchain()
    compiler = replace(FixtureCompiler.from_environment(), compiler=clang,
                       frontend="GNU", compiler_id="AppleClang")
    compiler.build([source], executable, Path(source).parent, includes=includes,
                   compile_options=["-isysroot", sdk, "-fobjc-arc", "-fblocks"],
                   link_options=["-isysroot", sdk, "-framework", "Foundation"])
