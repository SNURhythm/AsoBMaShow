# PR Review Shader and IME Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ensure shader CI always regenerates every backend and make the text-input clear button cancel the platform IME composition.

**Architecture:** Keep shader regeneration in the existing `shader_src/make.py` entrypoint: extend its clean operation to DX11 and make the workflow clean before compiling. At the UI boundary, call SDL's existing composition-cancellation API from the common clear-button path and intercept that external call only in the focused test executable.

**Tech Stack:** Python 3, GitHub Actions, CMake/CTest, C++23, SDL2, bgfx.

## Global Constraints

- Continue in-place on the existing `fix/ui-alignment` branch.
- Address only the two newly approved unresolved review threads.
- Regenerate Metal, SPIR-V, ESSL, and DX11 shader outputs on every shader workflow run.
- Keep incremental shader compilation behavior unchanged outside the workflow clean step.
- Clear both the common `TextInputBox` state and SDL's platform IME state.
- Do not reply to or resolve GitHub review threads without explicit authorization.

---

### Task 1: Force complete shader regeneration in CI

**Files:**
- Modify: `scripts/check_shader_compile_workflow.py`
- Modify: `shader_src/make.py`
- Modify: `.github/workflows/compile-shaders.yml`
- Test: `scripts/check_shader_compile_workflow.py`

**Interfaces:**
- Consumes: `python3 make.py clean` from the `shader_src` working directory.
- Produces: removal of `../shaders/metal`, `../shaders/spirv`, `../shaders/essl`, and `../shaders/dx11` before `python3 make.py` compiles all Windows-supported outputs.

- [ ] **Step 1: Add a failing workflow and clean-behavior audit**

Require the workflow command sequence and execute the real clean command against temporary shader directories:

```python
    "forced shader regeneration": "python3 make.py clean\n          python3 make.py",
```

```python
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
        [sys.executable, str(root / "shader_src" / "make.py"), "clean"],
        cwd=shader_source,
        env=clean_environment,
        check=True,
    )
    for backend in backends:
        if (shader_root / backend).exists():
            failures.append(f"clean command preserves {backend} output")
```

- [ ] **Step 2: Run the focused audit and verify RED**

Run:

```bash
python3 scripts/check_shader_compile_workflow.py .
```

Expected: exit code `1`, reporting the missing forced-regeneration command and preserved DX11 output.

- [ ] **Step 3: Extend clean and invoke it in the workflow**

Add DX11 to the existing clean branch:

```python
        shutil.rmtree("../shaders/dx11", ignore_errors=True)
```

Run clean before compilation:

```yaml
          python3 make.py clean
          python3 make.py
```

- [ ] **Step 4: Run the focused audit and verify GREEN**

Run:

```bash
python3 scripts/check_shader_compile_workflow.py .
ruby -e 'require "yaml"; YAML.parse_file(ARGV.fetch(0))' .github/workflows/compile-shaders.yml
```

Expected: the audit prints `shader compile workflow audit passed`, and Ruby exits `0` after parsing the workflow.

### Task 2: Cancel platform IME composition from the clear button

**Files:**
- Modify: `tests/text_input_box_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/view/TextInputBox.cpp`
- Test: `text_input_box_tests`

**Interfaces:**
- Consumes: a click on a visible clear button owned by `TextInputBox`.
- Produces: exactly one `SDL_ClearComposition()` call before publishing the empty editing value.

- [ ] **Step 1: Intercept the SDL boundary in the focused test executable**

Rename only the test target's SDL symbol and provide a counting replacement:

```cmake
    target_compile_definitions(text_input_box_tests PRIVATE
        SDL_ClearComposition=TextInputBoxTest_ClearComposition
    )
```

```cpp
namespace {
int clearCompositionCalls = 0;
}

extern "C" void TextInputBoxTest_ClearComposition() {
  ++clearCompositionCalls;
}
```

Reset the counter before clicking in `testClearButtonVisibilityAndCallback()` and assert it is `1` afterward:

```cpp
  clearCompositionCalls = 0;
  click(input, input.getX() + input.getWidth() - 18,
        input.getY() + input.getHeight() / 2);
  expect(clearCompositionCalls == 1,
         "clear button cancels the platform IME composition");
```

- [ ] **Step 2: Build and run the focused test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target text_input_box_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^text_input_box_tests$'
```

Expected: the executable builds, then fails with `clear button cancels the platform IME composition`.

- [ ] **Step 3: Add the minimal platform cancellation**

In `TextInputBox::clearFromButton()`, cancel the SDL composition alongside the existing internal reset:

```cpp
  editingText.clear();
  clearComposition();
  SDL_ClearComposition();
```

- [ ] **Step 4: Run the focused test to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target text_input_box_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^text_input_box_tests$'
```

Expected: CTest reports `1/1` passing.

### Task 3: Verify, commit, and push the review fixes

**Files:**
- Verify all files changed by Tasks 1 and 2.

**Interfaces:**
- Consumes: the two green focused fixes.
- Produces: a tested commit pushed to `origin/fix/ui-alignment`.

- [ ] **Step 1: Run the full local verification**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
```

Expected: the desktop target builds, all CTest tests pass, and `git diff --check` emits no output.

- [ ] **Step 2: Review and commit only the approved scope**

Run:

```bash
git status --short
git diff -- .github/workflows/compile-shaders.yml shader_src/make.py scripts/check_shader_compile_workflow.py CMakeLists.txt src/view/TextInputBox.cpp tests/text_input_box_tests.cpp docs/superpowers/plans/2026-07-25-pr-review-shader-ime-fixes.md
git add .github/workflows/compile-shaders.yml shader_src/make.py scripts/check_shader_compile_workflow.py CMakeLists.txt src/view/TextInputBox.cpp tests/text_input_box_tests.cpp docs/superpowers/plans/2026-07-25-pr-review-shader-ime-fixes.md
git commit -m "fix: address shader and IME review feedback"
```

Expected: one commit containing only the approved review fixes, focused regression coverage, and this plan.

- [ ] **Step 3: Push and confirm branch synchronization**

Run:

```bash
git push -u origin fix/ui-alignment
git status --short --branch
```

Expected: the push succeeds and the local branch is synchronized with `origin/fix/ui-alignment`.
