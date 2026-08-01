# Windows Shader Compiler Cache Design

## Goal

Reduce the Windows shader workflow's warm-run time by reusing the previously built bgfx `shaderc` executable while preserving exact dependency invalidation and the existing shader generation behavior.

## Chosen cache scope

Cache only `bgfx/bgfx/.build/win64_mingw-gcc/bin/shadercRelease.exe` with `actions/cache@v5`.

This is the smallest useful cache boundary. `shader_src/make.py` already treats that exact executable as the build-complete marker: when it exists, the script skips bgfx's expensive `make shaderc` invocation and immediately uses it to compile the project shaders. The executable is approximately 12 MB on the comparable local build, while the full bgfx object tree is much larger and carries more toolchain and incremental-build state.

Two alternatives were rejected:

- caching the complete bgfx `.build` tree would consume substantially more storage and could restore stale intermediate objects;
- adding `ccache` would improve incremental compilation across dependency changes, but it adds another compiler-cache configuration and still requires bgfx's build and link steps. It is disproportionate when the reusable output is one executable.

## Cache identity and invalidation

After recursive checkout, a Git-for-Windows Bash step reads the exact checked-out commits for `bgfx/bgfx`, `bgfx/bimg`, and `bgfx/bx`. It publishes their concatenation as a step output.

The cache key combines:

- the Windows Server 2022 / MINGW64 cache schema name;
- an explicit schema version for manual invalidation;
- the three submodule commit IDs.

The workflow does not define `restore-keys`. A partial match could restore a compiler built from different bgfx, bimg, or bx sources, so only an exact dependency match is safe. Changing any dependency pointer or deliberately incrementing the schema version causes a clean compiler build and a new cache entry.

## Workflow behavior

The cache restore step runs after checkout and before shader compilation. MSYS2 setup remains unchanged because the workflow still needs MinGW Python and its compiler preflight, and a cache miss still needs GNU Make and MinGW GCC.

On an exact cache hit, `shader_src/make.py` finds `shadercRelease.exe` and skips building it. On a miss, the script builds the compiler exactly as it does today. If the job succeeds, the cache action's post-job behavior saves the executable under the exact key. A failed compiler or shader build does not publish a cache.

Shader output staging, no-op detection, bot commits, branch concurrency, and same-branch pushes remain unchanged.

## Testing and verification

Extend the existing workflow audit before changing the workflow. The audit must require:

- a cache-key step containing all three `git rev-parse` commands;
- `actions/cache@v5` with a stable step ID;
- the exact `shadercRelease.exe` path and dependency-derived key;
- no `restore-keys` fallback.

Run the extended audit first and observe the expected failure, then implement the workflow and rerun the audit. Parse the YAML, build `main`, run the focused CTest, and run the complete CTest suite.

After pushing, verify the first GitHub-hosted run succeeds and saves the new compiler cache. Rerun the same workflow revision to prove an exact cache hit; the second compile step must skip bgfx's `make shaderc` output and be materially faster while still confirming that checked-in shaders are already current.
