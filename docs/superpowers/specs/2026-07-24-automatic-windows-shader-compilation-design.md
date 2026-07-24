# Automatic Windows Shader Compilation Design

## Goal

Keep checked-in shader binaries synchronized with `shader_src/` by running the existing compilation script on GitHub-hosted Windows infrastructure and committing generated changes back to the branch that triggered the workflow.

## Trigger and branch behavior

Create `.github/workflows/compile-shaders.yml` with `push` and `workflow_dispatch` triggers. Push runs are path-filtered to shader sources, the shader compilation workflow, the bgfx submodule pointer, and `.gitmodules`. The generated `shaders/` directory is deliberately excluded from the trigger paths, so a generated-binary-only commit does not request another compilation run.

The workflow runs on every pushed branch rather than only `main` or `develop`. Checkout explicitly selects `${{ github.ref_name }}` so the job owns a normal local branch and can push `HEAD` back to that same remote branch. Pull-request events are excluded because fork pull requests have read-only credentials and GitHub checks out pull-request merge refs rather than a writable contributor branch.

Manual dispatch is available for rebuilding a selected branch even when no source path changed. The workflow never force-pushes. A protected branch that does not allow GitHub Actions to write fails at the push step instead of bypassing repository policy.

## Windows compilation environment

Run on the pinned `windows-2022` runner. The current `shader_src/make.py` detects Windows through `sys.platform == "win32"`, builds bgfx `shaderc` with the repository's existing `make shaderc` fallback, and adds DirectX 11 `s_5_0` outputs alongside Metal, SPIR-V, and ESSL outputs.

Use `msys2/setup-msys2@v2` with its MINGW64 shell and install GNU Make, MinGW-w64 GCC, and MinGW-w64 Python. This matches bgfx's Windows `gmake-mingw-gcc` build path and makes both `make` and `python3` available to the unchanged script. Checkout initializes recursive submodules so `bgfx/bgfx`, `bimg`, and `bx` are present.

Disable Git's automatic CRLF conversion before checkout. This follows the MSYS2 action's documented checkout guidance and prevents a Windows checkout from dirtying tracked text sources before the bot evaluates generated changes.

## Commit policy and permissions

Grant only `contents: write` to `GITHUB_TOKEN`. The checkout action retains that credential for the final push.

After successful compilation, configure the standard `github-actions[bot]` identity and stage only `shaders/`. If the staged diff is empty, print a message and exit successfully without creating a commit. Otherwise create one `chore: compile shaders` commit and push `HEAD` to `${{ github.ref_name }}`.

Source files, workflow files, submodule state, and unrelated working-tree changes are never staged. GitHub does not create a second workflow run for a push authenticated by the repository's `GITHUB_TOKEN`; the path filter also excludes generated binaries, giving the workflow two independent protections against self-triggering loops.

Use a branch-scoped concurrency group with cancellation disabled. This serializes compilation commits for the same branch so an older run cannot race a newer source push, while allowing different branches to compile independently.

## Testing and verification

Add a standard-library Python audit that reads `.github/workflows/compile-shaders.yml` and enforces the workflow contract:

- writable contents permission, Windows Server 2022, branch-scoped concurrency, push path filters, and manual dispatch;
- no pull-request trigger;
- CRLF protection and recursive explicit-branch checkout;
- MINGW64 setup with GNU Make, MinGW-w64 GCC, and MinGW-w64 Python;
- invocation of the unchanged `python3 make.py` from `shader_src/`;
- bot identity, `git add -- shaders`, empty-diff exit, fixed commit message, and same-branch push;
- no force-push syntax.

Register the audit in CTest. First add and run the audit while the workflow file is absent to observe the expected failure. Then add the minimal workflow, rerun the focused audit, parse the YAML locally, build `main`, and run the complete CTest suite.

The actual Windows shader compiler execution is verified only when the workflow runs on GitHub. The local audit proves the action wiring and safety contract without pretending that macOS executed the Windows-only DirectX compilation path.
