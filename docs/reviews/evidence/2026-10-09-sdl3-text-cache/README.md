# Reproduce the text-selection cache comparison

Use the [adaptation build instructions](../2026-10-09-sdl3-adaptation/README.md)
with **`3e09cbdc` instead of `53f52b9e`** for the before source snapshot, and this
report's commit for the after snapshot. Build the same `probe3` target twice;
finish compilation and tests before collecting samples. The original harness,
Release libraries and dependencies remain required.

Invoke the existing runner with the correct baseline label and a new output path:

```sh
python3 docs/reviews/evidence/2026-10-09-sdl3-adaptation/compare.py \
  --baseline 3e09cbdc \
  --before "$benchmark_scratch/probe-before" \
  --after "$benchmark_scratch/probe-after" \
  --output "$benchmark_scratch/raw.json" --pairs 20
```

Revision labels are declarations; independently verify the source snapshots and
compiler/link settings. This run reused the existing Release build serially,
copied the before executable before editing, then rebuilt the candidate. The
only sampled application source change is `measured-change.patch.gz`. `build.json`
records before/after source fingerprints and candidate compile commands; raw
metadata records binary, harness and font hashes plus exact dependency commits.

The retained run used an otherwise identical temporary runner with baseline
`3e09cbdc` substituted for the old hardcoded value. The committed runner now
accepts `--baseline` while preserving its previous default. No timing or analysis
logic changed. `candidate_diff_sha256` covers the production patch captured
before these documentation/runner edits.

`raw.json.gz` losslessly stores all 40 retained process results. Decode with
Python `gzip.open` or `gzip -dc`. All 15 summary medians and paired aggregate
checksums were checked independently from the raw records after collection.
