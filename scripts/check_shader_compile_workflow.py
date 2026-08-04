#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import sys
import tempfile


root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
workflow_path = root / ".github/workflows/compile-shaders.yml"
workflow = workflow_path.read_text(encoding="utf-8") if workflow_path.is_file() else ""
cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")

workflow_required = {
    "push trigger": "  push:\n",
    "all-branch trigger": '      - "**"',
    "shader source path filter": '      - "shader_src/**"',
    "workflow path filter": '      - ".github/workflows/compile-shaders.yml"',
    "bgfx path filter": '      - "bgfx"',
    "submodule metadata path filter": '      - ".gitmodules"',
    "manual trigger": "  workflow_dispatch:\n",
    "write permission": "  contents: write",
    "branch concurrency": "group: compile-shaders-${{ github.ref }}",
    "non-cancelling concurrency": "cancel-in-progress: false",
    "pinned Windows runner": "runs-on: windows-2022",
    "branch-only job guard": "if: startsWith(github.ref, 'refs/heads/')",
    "CRLF protection": "git config --global core.autocrlf input",
    "checkout action": "uses: actions/checkout@v4",
    "explicit branch checkout": "ref: ${{ github.ref_name }}",
    "complete branch history": "fetch-depth: 0",
    "recursive submodules": "submodules: recursive",
    "shader compiler cache-key step": "id: shaderc-cache-key",
    "bgfx cache-key revision": "git -C bgfx/bgfx rev-parse HEAD",
    "bimg cache-key revision": "git -C bgfx/bimg rev-parse HEAD",
    "bx cache-key revision": "git -C bgfx/bx rev-parse HEAD",
    "shader compiler cache action": "uses: actions/cache@v5",
    "shader compiler cache step": "id: shaderc-cache",
    "shader compiler cache path": "path: bgfx/bgfx/.build/win64_mingw-gcc/bin/shadercRelease.exe",
    "exact shader compiler cache key": (
        "key: shaderc-windows-2022-mingw64-v1-"
        "${{ steps.shaderc-cache-key.outputs.value }}"
    ),
    "MSYS2 action": "uses: msys2/setup-msys2@v2",
    "MINGW64 environment": "msystem: MINGW64",
    "GNU Make package": "make",
    "MinGW GCC package": "mingw-w64-x86_64-gcc",
    "MinGW Python package": "mingw-w64-x86_64-python",
    "MSYS2 run shell": "shell: msys2 {0}",
    "shader working directory": "working-directory: shader_src",
    "MinGW toolchain root": "MINGW: /mingw64",
    "MinGW compiler preflight": 'test -x "$MINGW/bin/x86_64-w64-mingw32-g++.exe"',
    "forced shader regeneration": "python3 make.py clean\n          python3 make.py",
    "current shader script": "python3 make.py",
    "shader manifest write verification": (
        "python3 ../scripts/verify_skin_shader_outputs.py --root .. "
        "--shader skin_quad --require-backends metal,spirv,essl,dx11 "
        "--write-manifest "
        "../tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json"
    ),
    "shader manifest read-only verification": (
        "python3 ../scripts/verify_skin_shader_outputs.py --root .. "
        "--shader skin_quad --require-backends metal,spirv,essl,dx11 "
        "--manifest "
        "../tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json"
    ),
    "unexpected shader-tree change rejection": "verify_skin_shader_outputs.py",
    "Git for Windows commit shell": "- name: Commit generated shaders\n        shell: bash",
    "bot name": 'git config user.name "github-actions[bot]"',
    "bot email": 'git config user.email "41898282+github-actions[bot]@users.noreply.github.com"',
    "shader and manifest staging": (
        "git add -- shaders "
        "tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json"
    ),
    "empty-diff guard": "git diff --cached --quiet",
    "fixed commit message": 'git commit -m "chore: compile shaders"',
    "same-branch push": 'git push origin "HEAD:${{ github.ref_name }}"',
}

cmake_required = {
    "CTest registration": "add_test(NAME shader_compile_workflow_audit",
    "CTest audit command": "scripts/check_shader_compile_workflow.py",
}

failures = [
    label for label, fragment in workflow_required.items() if fragment not in workflow
]
failures.extend(
    label for label, fragment in cmake_required.items() if fragment not in cmake
)

workflow_sequence = (
    ("shader compilation", "          python3 make.py\n"),
    ("shader manifest write verification",
     workflow_required["shader manifest write verification"]),
    ("shader manifest read-only verification",
     workflow_required["shader manifest read-only verification"]),
    ("shader and manifest staging", workflow_required["shader and manifest staging"]),
)
sequence_end = -1
for label, fragment in workflow_sequence:
    position = workflow.find(fragment)
    if position < 0:
        continue
    if position <= sequence_end:
        failures.append(f"workflow runs {label} before shader compilation")
    sequence_end = position

with tempfile.TemporaryDirectory() as temp_dir:
    sandbox = Path(temp_dir)
    shader_source = sandbox / "shader_src"
    shader_source.mkdir()
    shader_root = sandbox / "shaders"
    backends = ("metal", "spirv", "essl", "dx11")
    for backend in backends:
        output = shader_root / backend
        output.mkdir(parents=True)
        (output / "stale.bin").write_bytes(b"stale")

    clean_environment = os.environ.copy()
    clean_environment["SHADERC"] = sys.executable
    subprocess.run(
        [sys.executable, str((root / "shader_src" / "make.py").resolve()),
         "clean"],
        cwd=shader_source,
        env=clean_environment,
        check=True,
    )
    for backend in backends:
        if (shader_root / backend).exists():
            failures.append(f"clean command preserves {backend} output")

for label, forbidden in {
    "pull-request trigger": "pull_request:",
    "force push": "--force",
    "generated shader trigger": '- "shaders/**"',
    "shader compiler fallback cache key": "restore-keys:",
}.items():
    if forbidden in workflow:
        failures.append(f"forbidden {label}")

if failures:
    for failure in failures:
        print(f"FAIL: shader compile workflow is missing {failure}", file=sys.stderr)
    raise SystemExit(1)

print("shader compile workflow audit passed")
