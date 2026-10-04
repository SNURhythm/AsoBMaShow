import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SHADERC = Path(os.environ.get(
    "SHADERC", ROOT / "bgfx/bgfx/.build/osx-arm64/bin/shadercRelease"))


class ShaderDependencyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "shader_src"
        self.source.mkdir()
        self.script = self.source / "make.py"
        shutil.copyfile(ROOT / "shader_src/make.py", self.script)
        self.compiler = self.root / "shaderc"
        self.compiler.write_text("compiler")
        self.vertex = self.source / "vs_test.sc"
        self.vertex.write_text('#include "shared.sh"\nvoid main() {}\n')
        self.shared = self.source / "shared.sh"
        self.shared.write_text('#include "nested.sh"\n')
        self.nested = self.source / "nested.sh"
        self.nested.write_text("// nested include\n")
        self.varying = self.source / "varying.def.sc"
        self.varying.write_text("vec3 a_position : POSITION;\n")
        self.output = self.root / "vs_test.bin"
        self.output.write_bytes(b"existing shader")
        for dependency in (self.script, self.compiler, self.vertex,
                           self.shared, self.nested, self.varying):
            os.utime(dependency, (100, 100))
        os.utime(self.output, (200, 200))
        with patch.dict(os.environ, {"SHADERC": str(self.compiler)}):
            spec = importlib.util.spec_from_file_location("shader_build", self.script)
            self.build = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.build)

    def test_unchanged_inputs_do_not_recompile(self):
        self.assertFalse(self.build.should_recompile_shader(self.vertex, self.output))

    def test_changed_dependencies_recompile(self):
        for dependency in (self.shared, self.nested, self.varying,
                           self.compiler, self.script):
            with self.subTest(dependency=dependency.name):
                os.utime(dependency, (300, 300))
                self.assertTrue(self.build.should_recompile_shader(
                    self.vertex, self.output))
                os.utime(dependency, (100, 100))

    def test_nested_shader_uses_its_own_varying_file(self):
        blur = self.source / "blur"
        blur.mkdir()
        vertex = blur / "vs_blur.sc"
        vertex.write_text('#include "../shared.sh"\nvoid main() {}\n')
        varying = blur / "varying.def.sc"
        varying.write_text("vec2 a_position : POSITION;\n")
        os.utime(vertex, (100, 100))
        os.utime(varying, (300, 300))
        self.assertTrue(self.build.should_recompile_shader(vertex, self.output))


class ShaderCleanTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("make"), "make is required")
    def test_clean_does_not_require_shader_compiler(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "shader_src"
            source.mkdir()
            for name in ("make.py", "Makefile"):
                shutil.copyfile(ROOT / "shader_src" / name, source / name)
            output = root / "shaders/spirv"
            output.mkdir(parents=True)
            (output / "stale.bin").write_bytes(b"stale")
            environment = os.environ.copy()
            environment.pop("SHADERC", None)
            result = subprocess.run(
                ["make", "clean", f"PYTHON={sys.executable}"], cwd=source,
                env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(output.exists())


@unittest.skipUnless(SHADERC.is_file() and shutil.which("make"),
                     "shaderc and make are required for compiler integration")
class ShaderMakeIntegrationTests(unittest.TestCase):
    def test_named_outputs_compile_the_corresponding_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "shader_src"
            shutil.copytree(ROOT / "shader_src", source)
            for backend, platform, profile in (
                    ("spirv", "windows", "spirv"),
                    ("metal", "osx", "metal"),
                    ("essl", "android", "310_es")):
                for shader in ("vs_text", "fs_text", "blur/vs_blur", "blur/fs_blurH"):
                    with self.subTest(backend=backend, shader=shader):
                        target = f"../shaders/{backend}/{shader}.bin"
                        result = subprocess.run(
                            ["make", "-B", f"SHADERC={SHADERC.resolve()}",
                             f"PYTHON={sys.executable}", target], cwd=source,
                            capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        expected = root / "expected.bin"
                        subprocess.run(
                            [str(SHADERC.resolve()), "-f", f"{shader}.sc", "-o", str(expected),
                             "--platform", platform, "--type", Path(shader).name[0],
                             "--profile", profile, "-O", "3", "-i", "."],
                            cwd=source, capture_output=True, check=True)
                        data = expected.read_bytes()
                        if backend == "essl":
                            data = data.replace(b"#version 310 es", b"#version 300 es")
                        self.assertEqual((source / target).read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
