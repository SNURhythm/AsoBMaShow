import argparse
import glob
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

# get shaderc from env "SHADERC"
root_path = f"{os.path.dirname(os.path.realpath(__file__))}/.."
bgfx_path = f"{root_path}/bgfx/bgfx"
bgfx_build_path = f"{bgfx_path}/.build/"
shaderc = os.getenv("SHADERC")
if shaderc is None:
    if sys.platform == "darwin":
        shaderc = f"{bgfx_build_path}/osx-arm64/bin/shadercRelease"
    elif sys.platform == "win32":
        shaderc = f"{bgfx_build_path}/win64_mingw-gcc/bin/shadercRelease.exe"
    else:
        shaderc = f"{bgfx_build_path}/linux64_gcc/bin/shadercRelease"
    # normalize path
    shaderc = os.path.abspath(shaderc)


def ensure_shader_compiler():
    # Cleaning and importing dependency checks must not build the toolchain.
    if os.getenv("SHADERC") is None:
        if not os.path.exists(shaderc):
            subprocess.run(["make", "-j14", "shaderc"], cwd=bgfx_path, check=True)
        print(f"Using shaderc: {shaderc}")


def should_recompile_shader(src, dst):
    if not os.path.exists(dst):
        return True
    source = Path(src).resolve()
    source_root = Path(root_path).resolve() / "shader_src"
    dependencies = {Path(__file__).resolve()}
    compiler_path = shutil.which(shaderc) or shaderc
    dependencies.add(Path(compiler_path).resolve())
    varying = source.parent / "varying.def.sc"
    if varying.is_file():
        dependencies.add(varying)
    pending = [source]
    while pending:
        dependency = pending.pop()
        if dependency in dependencies:
            continue
        dependencies.add(dependency)
        for include in re.findall(
                r'^\s*#\s*include\s*[<"]([^>"]+)[>"]',
                dependency.read_text(encoding="utf-8"), re.MULTILINE):
            candidates = (dependency.parent / include, source_root / include)
            resolved = next((path.resolve() for path in candidates if path.is_file()), None)
            if resolved is None:
                raise RuntimeError(f"Missing shader include {include} from {dependency}")
            pending.append(resolved)
    output_time = os.path.getmtime(dst)
    return any(path.stat().st_mtime > output_time for path in dependencies)


def essl_shader_needs_recompile(src, dst):
    if should_recompile_shader(src, dst):
        return True
    with open(dst, "rb") as f:
        data = f.read()
    return b"(-1.0/0.0)" in data or b"#version 300 es" not in data


def patch_essl_shader(dst):
    with open(dst, "rb") as f:
        data = f.read()

    data = data.replace(b"#version 310 es", b"#version 300 es")
    if b"(-1.0/0.0)" in data:
        raise RuntimeError(f"Invalid ESSL shader constants remain in {dst}")
    if b"#version 300 es" not in data:
        raise RuntimeError(f"Missing ESSL 300 version marker in {dst}")

    with open(dst, "wb") as f:
        f.write(data)


def compile_shader(src, dst, type, platform, profile):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    result = subprocess.run(
        [shaderc, "-f", src, "-o", dst, "--platform", platform, "--type", type, "--profile", profile, "-O", "3", "-i", "."]
    )
    if result.returncode != 0:
        print(f"Failed to compile shader {src} to {dst}")
        print(result.stdout)
        print(result.stderr)
        exit(1)


BACKENDS = {
    "metal": ("osx", "metal"),
    "spirv": ("windows", "spirv"),
    "essl": ("android", "310_es"),
    "dx11": ("windows", "s_5_0"),
}


def compile_all_shaders(backends=None, shaders=None):
    if backends is None:
        backends = ["metal", "spirv", "essl"]
        if sys.platform == "win32":
            backends.append("dx11")
    if "dx11" in backends and sys.platform != "win32":
        raise RuntimeError("DX11 shaders must be compiled on Windows")
    if shaders is None:
        shaders = sorted(glob.glob("**/fs_*.sc", recursive=True) +
                         glob.glob("**/vs_*.sc", recursive=True))
    for source in shaders:
        path = Path(source)
        if (path.is_absolute() or ".." in path.parts or not path.is_file() or
                path.suffix != ".sc" or not path.name.startswith(("vs_", "fs_"))):
            raise RuntimeError(f"Invalid shader source: {source}")
        print(source)
        for backend in backends:
            destination = str(Path("../shaders") / backend / path.with_suffix(".bin"))
            needs_recompile = (essl_shader_needs_recompile if backend == "essl"
                               else should_recompile_shader)
            if needs_recompile(source, destination):
                platform, profile = BACKENDS[backend]
                compile_shader(source, destination, path.name[0], platform, profile)
                if backend == "essl":
                    # shaderc 1.18.129 corrupts some ESSL 100/300 constants into
                    # -inf. Downshift the compatible 310_es output to GLES 3.0.
                    patch_essl_shader(destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Compile bgfx shaders")
    parser.add_argument("command", nargs="?", choices=["clean"])
    parser.add_argument("--backend", action="append", choices=BACKENDS)
    parser.add_argument("--shader", action="append", help="Source path relative to shader_src")
    args = parser.parse_args()
    if args.command == "clean":
        for backend in BACKENDS:
            shutil.rmtree(f"../shaders/{backend}", ignore_errors=True)
    else:
        ensure_shader_compiler()
        compile_all_shaders(args.backend, args.shader)
