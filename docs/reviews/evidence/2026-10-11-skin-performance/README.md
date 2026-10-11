# Skin performance evidence

- `raw.json.gz` and `summary.json`: all final loading/frame samples and derived medians/p95 values.
- `build.json.gz`: actual compile/link commands, machine/OS, final production source hashes and binary identities. The initial build manifest predates the benchmark's selector extension; `finalProbeCommands` records the final shared benchmark compilation and baseline link.
- `measured-change.patch.gz`: production changes against `9b3f966c`.
- `font-parity.json.gz`: matching input/output hashes for 63 local BMFont descriptors and 5,000 generated cases. `font-parity.cpp` reproduces that corpus and canonical result serialization.
- `validation.json`, compressed build/test logs: verification results, including the two allocation regressions before their fixes.
- `select-active.txt.gz` and `gameplay-active.txt.gz`: exploratory baseline stack samples, used to locate the catalog bottleneck. Their timings are not the final benchmark results.

Local third-party inputs were `~/Downloads/Skins/LITONE12` and
`~/Downloads/Skins/ModernChic`. The benchmark copies them to temporary storage;
it does not change or redistribute their contents. Reproduction requires those
local skin packages; they are not included in this repository.

From the commit containing this report, with a configured macOS Ninja build:

```sh
cmake --build cmake-build-debug --target gameplay_skin_loading_benchmark_tests -j 6
python3 docs/reviews/evidence/2026-10-11-skin-performance/build.py \
  --root "$PWD" --build-dir "$PWD/cmake-build-debug" \
  --output /tmp/skin-performance-repro --baseline 9b3f966c --candidate HEAD --font-parity
python3 docs/reviews/evidence/2026-10-11-skin-performance/compare.py \
  --output /tmp/skin-performance-repro --skin-root "$HOME/Downloads/Skins"
python3 docs/reviews/evidence/2026-10-11-skin-performance/verify-fonts.py \
  --output /tmp/skin-performance-repro --skin-root "$HOME/Downloads/Skins"
```

Use a new output directory outside the checkout. The builder snapshots sources
without creating worktrees and holds dependency archives fixed. Run comparisons
without concurrent builds/tests. The original run used equivalent temporary
build scripts; their executed commands are archived in `build.json.gz`.

The combined CLI is also available directly:

```sh
cmake-build-debug/gameplay_skin_loading_benchmark_tests --benchmark \
  --samples 9 --frame-samples 20000 --skin "$HOME/Downloads/Skins/LITONE12" \
  --entry Select/select.luaskin
```

`--benchmark --frame-samples` requires one `--entry` and reports both cold and
repeated loading regardless of `--mode`. Existing loading-only and acceptance
report modes retain their behavior.
