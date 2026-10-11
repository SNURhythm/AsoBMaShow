# Bounded ZIP asset input — 2026-10-11

The concurrent ZIP asset reader now feeds DEFLATE from at most 64 KiB of compressed scratch per active entry, instead of allocating the entire compressed entry. Its scheduling reservation uses output size plus that bounded scratch. Stored entries still read directly into their output buffer.

This path is used by chart audio rendering, jukebox loading and library scanning. This change does not modify the separate sequential miniz readers or disk extraction path.

Direct file mapping was considered but not used: ZIP sources can be truncated externally, and a mapped read could fault instead of returning the existing short-read error. Bounded reads preserve the platform-specific readAt implementation and avoid adding mapping lifetime or platform-specific fault handling.

## Correctness and verification

- Refill occurs only after tinfl consumes all buffered input. Remaining bytes stay available when decompression fills an output chunk first.
- The decompressor continues to consume valid empty DEFLATE blocks even after output is full. Input flags still reflect whether more compressed bytes remain.
- Output-size validation, CRC verification, cancellation checkpoints and allocation-failure handling remain in place.
- The new strict-budget regression failed against the previous implementation and passes now: a 65,537-byte output with more than 5 MiB compressed input fits output plus 64 KiB scratch. One byte less is rejected. Worker allocation instrumentation verifies the large scratch allocation is gone.
- Added a regression that truncates the source after output allocation and checks that the read fails without delivering an entry.
- Extended existing empty-block cancellation coverage to the concurrent reader. Existing boundary, CRC, oversized-entry scheduling and allocation-failure tests also pass.
- Desktop app and all configured targets built successfully. Full CTest: **481/481 passed**, using six parallel test jobs. The final test-state cleanup was rebuilt and the focused regressions rerun.
- Independent read-only review checked the refill state machine, bundled tinfl input lifetimes and memory reservations. No blocking findings remained.

## Benchmark

Baseline: `b397535b` (the actual previous ArchiveFile.cpp). Both readers, the benchmark harness and miniz bridge were compiled with `-O2` using the same dependency libraries. Twelve user-authorized local files (six WAV, six Ogg, about 4 KiB to 10 MB each) were placed into temporary DEFLATE ZIPs at compression level 6. The original assets were read-only. No private files or paths are committed.

Each sample warms the index and reader, then performs eight verified reads. Seven samples alternate baseline/current process order. Values below are medians of per-sample means. Every callback compares the entire extracted payload to the original file. The measured runs verified **4,032 entry deliveries**; warm-ups verified another 504.

Timings include exact byte comparison, public-reader preparation, thread creation and allocation instrumentation. They represent warmed verified extraction, not isolated inflate time or cold-storage performance. Ordinary C++ new/new[] allocations are counted; malloc allocations, page cache and process RSS are not. Allocation bytes are cumulative per pass, not peak resident memory. Single-entry cases request two workers because the public API requires it; internally they run one extraction worker.

| Case | Active workers | Output bytes | Old allocated bytes | New allocated bytes | Allocations old/new | Old ms | New ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| asset-0 | 1 | 4,271 | 19,543 | 19,543 | 72/72 | 0.120 | 0.120 |
| asset-1 | 1 | 7,758 | 26,126 | 26,126 | 72/72 | 0.138 | 0.136 |
| asset-2 | 1 | 12,722 | 36,031 | 36,031 | 72/72 | 0.159 | 0.158 |
| asset-3 | 1 | 46,782 | 102,656 | 102,656 | 72/72 | 0.338 | 0.337 |
| asset-4 | 1 | 172,712 | 354,269 | 249,712 | 72/72 | 1.003 | 1.000 |
| asset-5 | 1 | 2,663,879 | 5,332,504 | 2,740,887 | 72/72 | 14.215 | 14.139 |
| asset-6 | 1 | 4,112 | 19,543 | 19,543 | 72/72 | 0.115 | 0.114 |
| asset-7 | 1 | 21,836 | 51,333 | 51,333 | 72/72 | 0.228 | 0.219 |
| asset-8 | 1 | 37,266 | 71,326 | 71,326 | 72/72 | 0.265 | 0.264 |
| asset-9 | 1 | 191,078 | 362,817 | 268,078 | 72/72 | 1.171 | 1.174 |
| asset-10 | 1 | 1,498,276 | 2,962,454 | 1,575,284 | 72/72 | 8.057 | 8.048 |
| asset-11 | 1 | 10,062,370 | 18,621,708 | 10,139,386 | 72/72 | 53.644 | 53.655 |
| batch | 2 | 14,723,062 | 27,870,950 | 15,210,545 | 159/159 | 64.550 | 64.326 |
| batch | 4 | 14,723,062 | 27,871,526 | 15,211,121 | 173/173 | 55.062 | 54.889 |

The four-worker batch allocated **45.4% fewer C++ bytes** (27,871,526 → 15,211,121), with the same 173 allocations. Total compressed-scratch allocation per batch fell from 13,099,672 to 439,267 bytes (96.6% less); that sum is not simultaneous live scratch. The largest entry’s scratch fell from 8,547,858 to 65,536 bytes. Four active DEFLATE workers now need at most 256 KiB of compressed scratch, excluding output and decoder state.

Batch medians changed by less than 0.4%, and the largest asset was effectively unchanged. Treat speed as flat; the supported benefit is lower memory allocation and lower scheduling reservations. More entries may fit concurrently under a tight real memory budget, but that workload-dependent throughput effect was not measured here.

## Reproduce

Build `archive_file_concurrency_tests` first, stop competing builds/tests, then run:

```sh
python3 scripts/benchmark_zip_assets.py --baseline b397535b \
  --files-from /path/to/private-assets.txt \
  --output /tmp/asobmashow-zip-benchmark
```

The asset list contains one local file path per line. The runner writes temporary ZIPs, optimized executables, source snapshot, compile/link manifest, raw logs and results JSON to the external output directory. Generated archives and benchmark programs are not deployment artifacts.
