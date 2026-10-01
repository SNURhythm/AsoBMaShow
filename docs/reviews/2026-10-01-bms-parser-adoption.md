# BMS parser adoption and benchmark

Adopted the exact regenerated amalgamation from `bms-parser-cpp`
`561dceb13a1627cf5fba1ee86d86177a3db09702`. This retains the text scan and
copy-reduction work from `31c5b2e`; the subsequently reverted modern-library
integration is not included. No new application dependency is required.

The previous application parser is taken from `486e375d`. The only production
changes are `src/bms_parser.hpp` and `src/bms_parser.cpp`, copied after successful
`make clean`, `make test`, and `make test_amalgamation` in the parser repository.
No parser code was edited manually in the game repository.

Further uncommitted parser work appeared during verification. This adoption and
all measurements remain pinned to the verified `561dceb` snapshot; that ongoing
work was left untouched.

## Results

On Apple M1 Pro, the six medium-size fixtures (56,697–102,167 bytes) used
**18.2% less full-parse CPU time** and **20.0% less metadata-parse time**.
Their file-based full parsing used **17.2% less time** with warm filesystem
caches. The 8.4 MB `ubmchallenge` fixture improved by **29.5%** in full parsing.

The table sums each chart's seven-sample median, in milliseconds. These are
parser workloads, not complete game startup or database-import measurements.
The 8.4 MB chart dominates the all-fixture totals, so the medium-size subset is
reported separately.

| Corpus | Mode | Previous ms | Adopted ms | Time reduction |
| --- | --- | ---: | ---: | ---: |
| Six medium-size fixtures | full | 14.18 | 11.59 | 18.2% |
| Six medium-size fixtures | metadata | 9.61 | 7.68 | 20.0% |
| Six medium-size fixtures | file | 14.43 | 11.95 | 17.2% |
| All 13 repository fixtures | full | 124.68 | 89.50 | 28.2% |
| All 13 repository fixtures | metadata | 117.35 | 84.41 | 28.1% |
| All 13 repository fixtures | file | 125.44 | 90.94 | 27.5% |
| Seven generated cases | full | 36.51 | 29.73 | 18.6% |
| Seven generated cases | metadata | 26.63 | 22.75 | 14.6% |
| Seven generated cases | file | 36.64 | 30.36 | 17.1% |

Selected full-parse medians:

| Case | Bytes | Previous ms | Adopted ms | Time reduction |
| --- | ---: | ---: | ---: | ---: |
| `fixture_regression_altale_a` | 65,894 | 1.652 | 1.373 | 16.9% |
| `fixture_regression_jack_the_ripper_7normal` | 84,000 | 2.583 | 1.932 | 25.2% |
| `fixture_regression_ozma_spn` | 56,697 | 1.926 | 1.570 | 18.5% |
| `fixture_regression_wizdomiot_7another` | 102,167 | 2.878 | 2.353 | 18.2% |
| `fixture_aleph0_another` | 85,763 | 3.062 | 2.581 | 15.7% |
| `fixture_example` | 73,937 | 2.080 | 1.784 | 14.2% |
| `fixture_ubmchallenge` | 8,441,623 | 110.371 | 77.785 | 29.5% |
| `synthetic_ascii_dense` | 174,153 | 6.021 | 4.878 | 19.0% |
| `synthetic_utf8` | 174,205 | 5.713 | 4.802 | 16.0% |
| `synthetic_shiftjis` | 174,177 | 6.115 | 4.967 | 18.8% |
| `synthetic_euckr` | 174,173 | 6.096 | 5.178 | 15.1% |

All 60 case/mode comparisons had lower candidate medians in this run. The
smallest fixtures improved only slightly and their sample ranges overlap;
those differences should not be treated as reliable speedups. For example,
the 78-byte metadata case ranged from 12.13–18.33 µs before and 11.95–17.63 µs
after. Raw timing samples are [in the CSV](2026-10-01-bms-parser-samples.csv).

## Method

- Apple M1 Pro, macOS 26.5.1, Apple Clang 21.0.0.
- Both independent executables use the same harness and flags:
  `-std=c++23 -O3 -g0 -DNDEBUG -DBMS_PARSER_VERBOSE=0 -pthread`. No LTO.
- Twenty charts: ten upstream fixtures, three game fixtures, and seven generated
  cases covering sparse/dense ASCII, declared UTF-8, Shift-JIS, EUC-KR, automatic
  Shift-JIS detection, and a late charset declaration without a final newline.
- Seven samples, shuffled case/mode and backend order with a fixed seed.
  Each timed call warms up three times, then parses 3–100 times depending on
  chart byte size. Parser random seed is fixed at `20261001`.
- `full` and `metadata` preload bytes outside the measured region. `file` uses
  the filesystem API and warm-cache reads. All modes include parser construction,
  hashing, chart allocation, and chart/parser destruction. Chart assets are not
  loaded. Hash/digest comparison itself runs outside timing.
- The digest covers metadata, sorted resource tables, measures, timelines,
  normal/invisible/background/mine notes, and long-note partner links. Four modes
  are checked for equivalence, including ready-measure insertion; three are timed.
- Final timing ran after this task's builds and tests completed. This is a
  desktop benchmark, not a cold-disk or mobile performance measurement.

## Verification

- Upstream modular and amalgamated test suites passed after a clean regeneration.
- Both adopted files matched the generated artifacts byte-for-byte at adoption.
- All 80 old/new chart digest comparisons matched (20 charts × four modes).
- All 80 candidate corpus/mode parses passed AddressSanitizer and
  UndefinedBehaviorSanitizer.
- Desktop application and all test targets built successfully.
- The parser compiled for iOS ARM64 and Android ARM64 (NDK 28.2.13676358).
  Full mobile application builds and device benchmarks were not run.

The full 402-case CTest run had 399 passes, two failures, and one dependent
check not run:

- `play_skin_session_tests`: the selected-stage/banner-above-32-MiB assertion
  failed. This test allows two seconds for asynchronous image preparation.
- `foundation_profile_settings_persistence`: the assertion that `beginCommit`
  returns before `store.entered` becomes true failed. The test reads that flag
  without synchronization after a background worker has been notified, so the
  assertion can race the worker.
- `beatoraja_music_select_skin_ledger_evidence_contract` did not run because
  its `play_skin_session_tests` dependency failed.

A focused serial rerun, including the required ledger fixtures, passed all
23 tests without source changes. The initial failures are retained here;
this change does not fix those existing timing/concurrency-sensitive checks.

## Reproduce

With the sibling parser checkout available for its chart fixtures:

```sh
python3 scripts/benchmark_bms_parser.py --baseline 486e375d --build-only
# Complete other builds/tests before collecting timings.
python3 scripts/benchmark_bms_parser.py --skip-build
```

The script uses external source snapshots instead of worktrees and writes
`manifest.json`, `results.json`, and `samples.csv` under
`/tmp/asobmashow-parser-adoption`. The manifest records source and binary hashes,
compiler commands, the upstream revision, and corpus paths/sizes/hashes.
`--verify-only --skip-build` reruns just the semantic comparisons. `--output`,
`--parser-repo`, and `--samples` are configurable. Reproducing the original corpus
requires the fixture contents from the recorded upstream revision.

Adopted source SHA-256:

- `bms_parser.hpp`: `9648a0c6077d18941b2219e08c1e3262c3e6dbda970cb9debee80edeb492a338`
- `bms_parser.cpp`: `4f696652e1cb4897f1a37c1778f2ed8b1c7061a5b6776a5359f40f5d69956fab`
