import glob
import os
import shutil
import subprocess
import sys

# get shaderc from env "SHADERC"
root_path = f"{os.path.dirname(os.path.realpath(__file__))}/.."
current_path = os.getcwd()
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
    # if shaderc doesn't exist, try to build with make shaderc
    if not os.path.exists(shaderc):
        os.chdir(bgfx_path)
        subprocess.run(["make", "-j14", "shaderc"], check=True)
        os.chdir(current_path)
    # normalize path
    shaderc = os.path.abspath(shaderc)
    print(f"Using shaderc: {shaderc}")


def should_recompile_shader(src, dst):
    if not os.path.exists(dst):
        return True
    return os.path.getmtime(src) > os.path.getmtime(dst)


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
        [shaderc, "-f", src, "-o", dst, "--platform", platform, "--type", type, "--profile", profile, "-O 3", "-i", "."]
    )
    if result.returncode != 0:
        print(f"Failed to compile shader {src} to {dst}")
        print(result.stdout)
        print(result.stderr)
        exit(1)


def compile_all_shaders():
    # fs_*.sc : fragment shaders
    # vs_*.sc : vertex shaders

    # glob recursively
    fs_shaders = glob.glob("**/fs_*.sc", recursive=True)
    vs_shaders = glob.glob("**/vs_*.sc", recursive=True)
    compile_dx11 = sys.platform == "win32"

    for fs_shader in fs_shaders:
        if fs_shader.endswith("def.sc"):
            continue
        print(fs_shader)
        dst = "../shaders/metal/" + fs_shader.replace(".sc", ".bin")
        if should_recompile_shader(fs_shader, dst):
            compile_shader(fs_shader, dst, "f", "osx", "metal")
        dst = "../shaders/spirv/" + fs_shader.replace(".sc", ".bin")
        if should_recompile_shader(fs_shader, dst):
            compile_shader(fs_shader, dst, "f", "windows", "spirv")
        dst = "../shaders/essl/" + fs_shader.replace(".sc", ".bin")
        if essl_shader_needs_recompile(fs_shader, dst):
            # shaderc 1.18.129 corrupts some ESSL 100/300 float constants into
            # -inf. Compile through 310_es, then downshift compatible GLSL to
            # 300_es for Android GLES 3.0.
            compile_shader(fs_shader, dst, "f", "android", "310_es")
            patch_essl_shader(dst)
        dst = "../shaders/dx11/" + fs_shader.replace(".sc", ".bin")
        if compile_dx11 and should_recompile_shader(fs_shader, dst):
            compile_shader(fs_shader, dst, "f", "windows", "s_5_0")
        elif not compile_dx11 and not os.path.exists(dst):
            print(f"Skipping DX11 shader on this platform: {dst}")

    for vs_shader in vs_shaders:
        if vs_shader.endswith("def.sc"):
            continue
        print(vs_shader)
        dst = "../shaders/metal/" + vs_shader.replace(".sc", ".bin")
        if should_recompile_shader(vs_shader, dst):
            compile_shader(vs_shader, dst, "v", "osx", "metal")
        dst = "../shaders/spirv/" + vs_shader.replace(".sc", ".bin")
        if should_recompile_shader(vs_shader, dst):
            compile_shader(vs_shader, dst, "v", "windows", "spirv")
        dst = "../shaders/essl/" + vs_shader.replace(".sc", ".bin")
        if essl_shader_needs_recompile(vs_shader, dst):
            # shaderc 1.18.129 corrupts some ESSL 100/300 float constants into
            # -inf. Compile through 310_es, then downshift compatible GLSL to
            # 300_es for Android GLES 3.0.
            compile_shader(vs_shader, dst, "v", "android", "310_es")
            patch_essl_shader(dst)
        dst = "../shaders/dx11/" + vs_shader.replace(".sc", ".bin")
        if compile_dx11 and should_recompile_shader(vs_shader, dst):
            compile_shader(vs_shader, dst, "v", "windows", "s_5_0")
        elif not compile_dx11 and not os.path.exists(dst):
            print(f"Skipping DX11 shader on this platform: {dst}")


if __name__ == "__main__":
    args = sys.argv[1:]
    if len(args) > 0 and args[0] == "clean":
        shutil.rmtree("../shaders/metal", ignore_errors=True)
        shutil.rmtree("../shaders/spirv", ignore_errors=True)
        shutil.rmtree("../shaders/essl", ignore_errors=True)
        shutil.rmtree("../shaders/dx11", ignore_errors=True)
    else:
        compile_all_shaders()
