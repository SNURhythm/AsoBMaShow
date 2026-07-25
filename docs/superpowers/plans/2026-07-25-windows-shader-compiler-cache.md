# Windows Shader Compiler Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make repeat Windows shader workflow runs reuse the exact previously built bgfx `shadercRelease.exe` instead of rebuilding the compiler.

**Architecture:** The workflow derives a cache identity from the checked-out bgfx, bimg, and bx commits, then restores one executable at the path already recognized by `shader_src/make.py`. The existing local workflow audit enforces the cache path, exact invalidation inputs, action version, and absence of unsafe prefix fallbacks.

**Tech Stack:** GitHub Actions, `actions/cache@v5`, Git for Windows Bash, MSYS2 MINGW64, Python 3, CMake/CTest, git.

## Global Constraints

- Continue in-place on the existing `fix/ui-alignment` branch.
- Cache only `bgfx/bgfx/.build/win64_mingw-gcc/bin/shadercRelease.exe`.
- Key the cache on all three checked-out commits: `bgfx/bgfx`, `bgfx/bimg`, and `bgfx/bx`.
- Use a versioned Windows Server 2022 / MINGW64 key and `actions/cache@v5`.
- Do not configure `restore-keys`; only an exact dependency match is safe.
- Keep `shader_src/make.py`, MSYS2 setup, shader staging, no-op detection, bot commit identity, and same-branch push behavior unchanged.
- Do not push or save a cache from a failed job.

---

### Task 1: Enforce the shader compiler cache contract

**Files:**
- Modify: `scripts/check_shader_compile_workflow.py`
- Test: `scripts/check_shader_compile_workflow.py`

**Interfaces:**
- Consumes: `.github/workflows/compile-shaders.yml` as UTF-8 text.
- Produces: exit code `1` for any missing cache contract or forbidden fallback; exit code `0` and `shader compile workflow audit passed` after the workflow is compliant.

- [ ] **Step 1: Add the cache requirements to the workflow audit**

Add these entries to `workflow_required` after the recursive-submodule assertion:

```python
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
```

Add this entry to the existing forbidden-fragment mapping:

```python
    "shader compiler fallback cache key": "restore-keys:",
```

- [ ] **Step 2: Run the audit to verify the new contract fails**

Run:

```bash
python3 scripts/check_shader_compile_workflow.py .
```

Expected: exit code `1` with failures for `shader compiler cache-key step`, all three cache-key revisions, the cache action and step, the cache path, and the exact cache key.

### Task 2: Restore and save the exact shader compiler

**Files:**
- Modify: `.github/workflows/compile-shaders.yml`
- Test: `scripts/check_shader_compile_workflow.py`

**Interfaces:**
- Consumes: the checked-out commits at `bgfx/bgfx`, `bgfx/bimg`, and `bgfx/bx`.
- Produces: a cache entry containing only `bgfx/bgfx/.build/win64_mingw-gcc/bin/shadercRelease.exe`, or restores that executable on an exact cache hit.

- [ ] **Step 1: Add the cache identity and restore steps after checkout**

Insert these steps between `Checkout triggering branch` and `Set up MINGW64`:

```yaml
      - name: Resolve shader compiler cache key
        id: shaderc-cache-key
        shell: bash
        run: |
          echo "value=$(git -C bgfx/bgfx rev-parse HEAD)-$(git -C bgfx/bimg rev-parse HEAD)-$(git -C bgfx/bx rev-parse HEAD)" >> "$GITHUB_OUTPUT"

      - name: Restore shader compiler
        id: shaderc-cache
        uses: actions/cache@v5
        with:
          path: bgfx/bgfx/.build/win64_mingw-gcc/bin/shadercRelease.exe
          key: shaderc-windows-2022-mingw64-v1-${{ steps.shaderc-cache-key.outputs.value }}
```

Do not add `restore-keys`. Leave all later MSYS2, compilation, and commit steps unchanged.

- [ ] **Step 2: Run the focused checks**

Run:

```bash
python3 scripts/check_shader_compile_workflow.py .
ruby -e 'require "yaml"; YAML.parse_file(ARGV.fetch(0))' .github/workflows/compile-shaders.yml
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R shader_compile_workflow_audit
```

Expected: the direct audit passes, Ruby exits `0`, `main` builds, and CTest reports `1/1` passing.

- [ ] **Step 3: Run the full local verification**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
git diff -- .github/workflows/compile-shaders.yml scripts/check_shader_compile_workflow.py
```

Expected: all tests pass, `git diff --check` emits no output, and the implementation diff contains only the cache contract and the two new workflow steps.

- [ ] **Step 4: Commit the implementation**

Run:

```bash
git add .github/workflows/compile-shaders.yml scripts/check_shader_compile_workflow.py
git commit -m "ci: cache Windows shader compiler"
```

Expected: one implementation commit with only the audit and workflow changes.

### Task 3: Prove cold- and warm-cache behavior on GitHub

**Files:**
- Verify only; no expected file changes.

**Interfaces:**
- Consumes: the pushed `fix/ui-alignment` workflow revision and its first successful GitHub Actions run.
- Produces: evidence that a miss builds and saves the compiler, while a rerun restores the exact cache and skips bgfx compiler compilation.

- [ ] **Step 1: Push the branch and monitor the cold-cache run**

Run:

```bash
git push origin fix/ui-alignment
gh run list --workflow compile-shaders.yml --branch fix/ui-alignment --limit 1
gh run watch RUN_ID --exit-status
```

Expected: the run succeeds; `Restore shader compiler` reports a miss, `Compile shaders` builds bgfx shaderc, and the cache post-step saves `shadercRelease.exe`. The shader commit step reports that binaries are already up to date.

- [ ] **Step 2: Rerun the identical revision and monitor the warm-cache run**

Run:

```bash
gh run rerun RUN_ID
gh run watch RUN_ID --exit-status
gh run view RUN_ID --log
```

Expected: `Restore shader compiler` reports an exact hit, `Compile shaders` has no bgfx `make shaderc` build output and completes materially faster than the cold run, and the job succeeds without a generated-shader commit.

- [ ] **Step 3: Confirm repository synchronization**

Run:

```bash
git fetch origin fix/ui-alignment
git status --short --branch
git log -3 --oneline
```

Expected: the local branch is synchronized with `origin/fix/ui-alignment` and the worktree is clean.
