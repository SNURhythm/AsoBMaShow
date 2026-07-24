# Automatic Windows Shader Compilation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compile all shader backends, including DirectX 11, with the existing shader script on Windows and commit changed generated binaries back to the triggering branch.

**Architecture:** A path-filtered GitHub Actions workflow checks out the triggering branch with recursive submodules, provisions a MINGW64 environment that matches bgfx's existing Windows Make target, and runs `shader_src/make.py` unchanged. A focused Python audit makes the credential, trigger, compiler, staging, and push contract locally testable before the workflow is published.

**Tech Stack:** GitHub Actions, Windows Server 2022, MSYS2 MINGW64, Python 3, GNU Make, MinGW-w64 GCC, bgfx shaderc, CMake/CTest, git.

## Global Constraints

- Continue in-place on the existing `fix/ui-alignment` branch.
- Run `shader_src/make.py` unchanged on Windows so `sys.platform == "win32"` enables DirectX 11 `s_5_0` compilation.
- Trigger on pushed shader inputs and manual dispatch, but not on pull-request events or generated `shaders/` changes.
- Restrict push triggers and job execution to branch refs so tags are never rewritten.
- Grant only `contents: write` and stage only `shaders/`.
- Never force-push; push `HEAD` to the branch identified by `${{ github.ref_name }}`.
- Do not create a commit when the staged shader diff is empty.
- Use the existing `github-actions[bot]` identity and `chore: compile shaders` commit message.

---

### Task 1: Add the shader workflow contract audit

**Files:**
- Create: `scripts/check_shader_compile_workflow.py`
- Modify: `CMakeLists.txt`
- Test: `scripts/check_shader_compile_workflow.py`

**Interfaces:**
- Consumes: repository root as optional argument one and `.github/workflows/compile-shaders.yml` beneath that root.
- Produces: exit code `0` plus `shader compile workflow audit passed` when every required workflow fragment is present; exit code `1` plus one `FAIL:` line per missing or forbidden behavior otherwise.

- [ ] **Step 1: Write the failing audit**

Create `scripts/check_shader_compile_workflow.py` with this complete implementation:

```python
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
    "current shader script": "run: python3 make.py",
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
```

Register the audit alongside the existing Python audits in `CMakeLists.txt`:

```cmake
    add_test(NAME shader_compile_workflow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_shader_compile_workflow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(shader_compile_workflow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
```

- [ ] **Step 2: Run the audit to verify it fails for the absent workflow**

Run:

```bash
python3 scripts/check_shader_compile_workflow.py .
```

Expected: exit code `1`; output includes `FAIL: shader compile workflow is missing push trigger`, `FAIL: shader compile workflow is missing pinned Windows runner`, and `FAIL: shader compile workflow is missing current shader script`.

### Task 2: Implement the Windows compilation and commit workflow

**Files:**
- Create: `.github/workflows/compile-shaders.yml`
- Test: `scripts/check_shader_compile_workflow.py`

**Interfaces:**
- Consumes: pushes changing `shader_src/**`, `.github/workflows/compile-shaders.yml`, `bgfx`, or `.gitmodules`, plus manual branch dispatches.
- Produces: updated tracked binaries under `shaders/` in a bot-authored `chore: compile shaders` commit on the triggering branch, or a successful no-op when generated binaries are unchanged.

- [ ] **Step 1: Add the minimal passing workflow**

Create `.github/workflows/compile-shaders.yml`:

```yaml
name: Compile shaders

on:
  push:
    branches:
      - "**"
    paths:
      - "shader_src/**"
      - ".github/workflows/compile-shaders.yml"
      - "bgfx"
      - ".gitmodules"
  workflow_dispatch:

permissions:
  contents: write

concurrency:
  group: compile-shaders-${{ github.ref }}
  cancel-in-progress: false

jobs:
  compile:
    name: Compile and commit shaders
    if: startsWith(github.ref, 'refs/heads/')
    runs-on: windows-2022
    timeout-minutes: 30

    steps:
      - name: Preserve repository line endings
        shell: pwsh
        run: git config --global core.autocrlf input

      - name: Checkout triggering branch
        uses: actions/checkout@v4
        with:
          ref: ${{ github.ref_name }}
          fetch-depth: 0
          submodules: recursive

      - name: Set up MINGW64
        uses: msys2/setup-msys2@v2
        with:
          msystem: MINGW64
          update: true
          install: >-
            make
            mingw-w64-x86_64-gcc
            mingw-w64-x86_64-python

      - name: Compile shaders
        working-directory: shader_src
        shell: msys2 {0}
        run: python3 make.py

      - name: Commit generated shaders
        shell: msys2 {0}
        run: |
          git config user.name "github-actions[bot]"
          git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
          git add -- shaders
          if git diff --cached --quiet; then
            echo "Shader binaries are already up to date."
            exit 0
          fi
          git commit -m "chore: compile shaders"
          git push origin "HEAD:${{ github.ref_name }}"
```

- [ ] **Step 2: Run the focused audit to verify it passes**

Run:

```bash
python3 scripts/check_shader_compile_workflow.py .
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R shader_compile_workflow_audit
```

Expected: direct audit prints `shader compile workflow audit passed`, `main` builds, and CTest reports `1/1` passing.

- [ ] **Step 3: Parse the workflow and inspect the implementation diff**

Run:

```bash
ruby -e 'require "yaml"; YAML.parse_file(ARGV.fetch(0))' \
  .github/workflows/compile-shaders.yml
git diff --check
git diff -- .github/workflows/compile-shaders.yml \
  scripts/check_shader_compile_workflow.py CMakeLists.txt
```

Expected: Ruby exits `0`, `git diff --check` has no output, and the diff contains only the workflow, its audit, and CTest registration.

- [ ] **Step 4: Commit the implementation**

Run:

```bash
git add .github/workflows/compile-shaders.yml \
  scripts/check_shader_compile_workflow.py CMakeLists.txt
git commit -m "ci: compile shaders on Windows"
```

Expected: one commit containing the workflow and its executable contract audit.

### Task 3: Verify and hand off the branch

**Files:**
- Verify only; no expected file changes.

**Interfaces:**
- Consumes: the complete `fix/ui-alignment` branch after the workflow implementation commit.
- Produces: fresh local build/test evidence and a clean committed branch ready for the existing pull request.

- [ ] **Step 1: Run complete local verification**

Run:

```bash
python3 scripts/check_shader_compile_workflow.py .
ruby -e 'require "yaml"; YAML.parse_file(ARGV.fetch(0))' \
  .github/workflows/compile-shaders.yml
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
git status --short --branch
```

Expected: both static checks exit `0`, `main` builds, all 141 CTest tests pass, the diff check is empty, and the branch has no uncommitted changes.

- [ ] **Step 2: Preserve the branch for user-directed publication**

Run:

```bash
git log --oneline origin/fix/ui-alignment..HEAD
```

Expected: the new design, plan, and workflow commits are listed locally. Do not push unless the user explicitly requests publication.
