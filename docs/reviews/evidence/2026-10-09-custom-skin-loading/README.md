# Native-row skin loading evidence

- `raw.json`: all sixteen measured invocations, including commands and samples.
- `summary.json`: medians and percentage reductions recomputed from those samples.
  Warm summaries exclude the first invocation of the session within each process.
- `build.json.gz`: actual optimized compile/link commands, CPU/OS, source and
  executable hashes. `variantCommands` recompiles the final shared benchmark
  fixture, then replaces only the candidate's image decoder object.
- `measured-change.patch.gz`: exact production diff measured against `1517a9237`.
- `inputs.json`: file counts, byte totals and tree digests for the local skins.
  Each digest hashes sorted relative paths, a NUL separator and each file's
  binary SHA-256 digest, without including skin contents in this evidence.
- `validation.json`: native checks and independent review findings.
- `build.py`, `compare.py`: parameterized reproduction of the build and workload.
  The original run used equivalent temporary scripts with local paths fixed.

The local skin inputs were `~/Downloads/Skins/LITONE12` and
`~/Downloads/Skins/ModernChic`, using their 7-key gameplay and music-select entries.
They are not redistributed here. The benchmark copies each package into its own
temporary fixture; Lua writes remain within that fixture. No source skin is edited.

From a checkout containing this change and a configured macOS debug Ninja build:

```sh
cmake --build cmake-build-debug --target gameplay_skin_loading_benchmark_tests -j 6
python3 docs/reviews/evidence/2026-10-09-custom-skin-loading/build.py \
  --root "$PWD" --build-dir "$PWD/cmake-build-debug" \
  --output /tmp/custom-skin-loading-repro \
  --baseline 1517a9237 --candidate HEAD
python3 docs/reviews/evidence/2026-10-09-custom-skin-loading/compare.py \
  --output /tmp/custom-skin-loading-repro --skin-root "$HOME/Downloads/Skins"
```

Use the commit containing this report as `--candidate` if HEAD includes later
production changes. The build script requires a new external output directory
and exactly the three measured production files in the comparison. It snapshots
sources without creating worktrees and reuses the configured dependency archives.
Do not run builds or other benchmarks concurrently with `compare.py`.
