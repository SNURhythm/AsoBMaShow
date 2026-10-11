# Mapped ZIP asset input — 2026-10-11

The concurrent ZIP reader now maps compressed input directly into the DEFLATE decoder. This follows the [bounded-input change](2026-10-11-zip-input-chunks.md) and compares mmap against that implementation, not the original entry-sized scratch allocation.

## Implementation

- One read-only mapping per concurrent read is shared by all workers. POSIX uses `mmap`; Windows uses `CreateFileMappingW` and `MapViewOfFile`. Stored entries keep reading directly into their output buffers.
- The file wrapper owns both the open handle and the mapping, verifies the current file size and uses the validated entry offsets. Size is capped at `PTRDIFF_MAX`. The owner outlives the workers and unmaps on return, cancellation or exception unwinding.
- DEFLATE consumes mapped bytes in bounded steps, preserving cancellation, decompressed-size validation and CRC verification. Successful mapping eliminates the compressed scratch allocation and read/copy calls for each deflated entry.
- Mapping failure or unsupported size falls back to the existing 64 KiB chunk reader. No mandatory mapping requirement is imposed on a filesystem or device.
- Scheduling retains the conservative output-plus-fallback-scratch reservation. Mapped file pages are managed by the OS; this limit is not an RSS cap.
- The archive must remain intact during a mapped read. Concurrent in-place truncation can fault on POSIX; this rare condition was explicitly accepted in choosing this path. No signal handler or snapshot copy was added.

## Verification

The no-scratch-allocation regression failed against the chunked reader and passes with mapping: a valid DEFLATE entry must extract correctly while the scratch-sized heap allocation is rejected. Existing boundary, empty-output, CRC, cancellation and parallel-reader tests cover the mapped path. The test-only mapping-disable switch exercises chunk fallback integrity, cancellation, budget enforcement and read-time truncation errors.

The desktop app and all configured targets built successfully. Full CTest passed **481/481 tests** with six parallel jobs (119.76 seconds).

Independent review found no blocking issues in mapping bounds, lifetime, cleanup or fallback handling. Verification was performed on the local macOS host; Windows and mobile runtime behavior were not exercised in this run.

## Benchmark method

Baseline: `47b9c009` (64 KiB chunked input). Both actual ArchiveFile.cpp implementations, the harness and miniz bridge are compiled at `-O2`, with identical dependency libraries. Each sample warms the reader and runs eight exact-byte-verified extractions. Seven samples alternate variant order; medians of per-sample means are shown. Builds/tests were stopped during timing.

Two private DEFLATE ZIP datasets were generated at compression level 6: the same 12 WAV/Ogg files used previously, and one 708-asset chart directory (the median entry count among 15 local directories with 100–1,000 audio assets). Originals were read-only. Private asset names, paths, archives and raw results remain outside the repository.

The measured runs checked **162,624 entry deliveries byte-for-byte** against their original files. Warm-ups also checked their outputs. Timings include reader preparation, mapping/unmapping, thread creation, byte comparisons and allocation instrumentation. Internal mmap page-fault cost can appear under inflate timing, so this report uses end-to-end time.

Allocation counters cover ordinary C++ new/new[] during each read. They exclude malloc and OS mappings. Byte counts are cumulative allocations per pass, not peak live bytes or RSS. The page cache is outside these measurements. Cold-cache performance was not measured.

## Batch results

| Dataset | Workers | Chunked ms | Mapped ms | Chunked allocations | Mapped allocations | Chunked allocated bytes | Mapped allocated bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| 12 assets | 2 | 64.420 | 64.091 | 159 | 147 | 15,210,545 | 14,771,278 |
| 12 assets | 4 | 54.839 | 54.873 | 173 | 161 | 15,211,121 | 14,771,854 |
| 708 assets | 2 | 40.706 | 40.254 | 5,017 | 4,309 | 25,948,313 | 15,975,558 |
| 708 assets | 4 | 22.241 | 21.912 | 5,031 | 4,323 | 25,948,937 | 15,976,182 |

For the 708-asset chart, mapping removes **708 allocations** and **9,972,755 allocated bytes per pass**: 38.4% fewer bytes and 14.1% fewer allocations with four workers. Median times were 1.1% lower with two workers and 1.5% lower with four; these small timing differences should not be treated as a broad speed guarantee.

For the 12-file batch, allocated bytes fell another 2.9% relative to chunks, with one fewer allocation per asset. Batch speed was effectively unchanged. Individual small-file reads sometimes became slightly slower due to mapping setup: for example, the smallest Ogg case changed from 0.115 to 0.125 ms. This implementation is retained for reduced copying and allocation, with the strongest measured benefit on batches of many small assets.

## Reproduce

Build `archive_file_concurrency_tests`, then run without competing builds/tests:

```sh
python3 scripts/benchmark_zip_assets.py --baseline 47b9c009 \
  --files-from /path/to/private-assets.txt \
  --output /tmp/asobmashow-zip-mmap-benchmark

python3 scripts/benchmark_zip_assets.py --baseline 47b9c009 \
  --files-from /path/to/private-chart-assets.txt --batch-only \
  --output /tmp/asobmashow-zip-mmap-chart-benchmark
```

The runner records source hashes, commands, raw output and JSON results externally. It now reports measured allocation bytes without deriving scratch estimates from assumptions about the baseline revision.
