# ZIP Apply traversal — 2026-10-18

Apply still validates every central and local record before mutation. It now
walks the central directory first, then validates local records in physical
offset order through the existing temporary SQLite manifest. Header/name and
descriptor reads are combined. Candidate and trailer traversal reuse prepared
queries, resetting each query before updating the manifest. The stem index is
built in bulk after validation.

The persistent browsing index format and contents are unchanged. Preparation
does not scan complete payloads or make a complete archive copy.

## Measurement

Run the opt-in benchmark with:

```sh
cmake --build cmake-build-debug --target storage_optimizer_tests -j 6
cmake-build-debug/storage_optimizer_tests --archive-traversal-benchmark 20000
```

The fixture has 20,000 stored WAV-named entries of 2,048 bytes each and a reversed
central directory. Timing excludes fixture creation. It covers manifest
construction and one candidate traversal, without encoding, journal publication,
or compaction. The candidate loop uses the same query strategy as Apply in each
version. These are single warm-cache runs on the local Apple M1 Pro/macOS debug
build, not device benchmarks or full-operation latency predictions.

| Metric | Before | After |
| --- | ---: | ---: |
| Archive read calls | 80,007 | 40,007 |
| Reads starting before the previous read's end | 40,001 | 6 |
| Archive bytes requested | 2,523,399 | 2,443,399 |
| Manifest preparation (ms) | 163.214 | 191.785 |
| Candidate traversal (ms) | 813.904 | 270.909 |
| Combined (ms) | 977.118 | 462.694 |

Combined time decreased about 53%, driven by query reuse. Preparation itself
increased about 18% on this cached filesystem: the extra manifest pass costs CPU
and SQLite work in exchange for fewer reads and physical-order local validation.
Read-call and ordering assertions use a 32-entry fixture in the regular suite;
wall time is deliberately not a test assertion.

## Correctness boundaries

Small fixtures cover signed/unsigned classic and ZIP64 descriptors, unsigned
lookahead into the next local record, a descriptor immediately before the central
directory, truncated/corrupt descriptors, and compaction with reversed central
order. Existing tests continue to cover stale input, cancellation, exact retained
payloads, ZIP64 offsets, and interrupted compaction/recovery.

Archive rows measure uncompressed contents, while reclaimed space measures the
actual archive file-size reduction. A deflated-fixture test verifies both totals;
the result UI now explains this distinction after an archive changes.
