#!/usr/bin/env python3

from pathlib import Path
import sys


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
    "MSYS2 action": "uses: msys2/setup-msys2@v2",
    "MINGW64 environment": "msystem: MINGW64",
    "GNU Make package": "make",
    "MinGW GCC package": "mingw-w64-x86_64-gcc",
    "MinGW Python package": "mingw-w64-x86_64-python",
    "MSYS2 run shell": "shell: msys2 {0}",
    "shader working directory": "working-directory: shader_src",
    "MinGW toolchain root": "MINGW: /mingw64",
    "MinGW compiler preflight": 'test -x "$MINGW/bin/x86_64-w64-mingw32-g++.exe"',
    "current shader script": "python3 make.py",
    "bot name": 'git config user.name "github-actions[bot]"',
    "bot email": 'git config user.email "41898282+github-actions[bot]@users.noreply.github.com"',
    "shader-only staging": "git add -- shaders",
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

for label, forbidden in {
    "pull-request trigger": "pull_request:",
    "force push": "--force",
    "generated shader trigger": '- "shaders/**"',
}.items():
    if forbidden in workflow:
        failures.append(f"forbidden {label}")

if failures:
    for failure in failures:
        print(f"FAIL: shader compile workflow is missing {failure}", file=sys.stderr)
    raise SystemExit(1)

print("shader compile workflow audit passed")
