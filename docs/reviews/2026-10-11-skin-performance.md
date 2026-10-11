# Music-select and gameplay skin performance

Music-select sessions now prepare their immutable renderer indexes once, as
owning gameplay sessions already do. Numeric selector admission uses the existing
indexed catalog instead of scanning all entries. That shared numeric index now
uses a 16-KiB membership table for common IDs, populated from the authoritative
catalog; sparse IDs and timer/event ranges retain their fallback rules. Named
selectors, state values, callback evaluation and draw order are unchanged.

Bitmap-font loading reuses a vector of borrowed attribute views across descriptor
lines, replacing per-field map nodes and owning strings. Reverse lookup preserves
last-duplicate semantics, including malformed or empty final values. Retained
page filenames remain owned strings.

## Measurements

Baseline: `9b3f966c`. Apple M1 Pro, macOS; application objects compiled with
`-O3 -DNDEBUG`, reusing identical dependencies from the configured debug tree.
The candidate replaces only the four changed implementation objects. The same
benchmark source and recording backend are used in both executables.

| Skin / screen | Cold load, ms (before → after) | Repeated load, ms (before → after) | CPU frame, µs (before → after) |
| --- | ---: | ---: | ---: |
| LITONE12 gameplay | 337.5 → 315.0 | 274.0 → 251.5 | 271.7 → 256.7 |
| LITONE12 music select | 272.4 → 254.7 | 252.0 → 228.4 | 970.5 → 246.9 |
| ModernChic gameplay | 447.7 → 447.4 | 81.4 → 83.5 | 61.5 → 62.5 |
| ModernChic music select | 263.8 → 263.9 | 139.9 → 139.3 | 2,997.2 → 418.1 |

Selector CPU medians fell **74.6% and 86.0%**. LITONE12 gameplay CPU time fell
**5.5%**, and its gameplay/select loading medians fell **6.5–9.4%**. ModernChic
loading shows little change; its gameplay median was **1.7% higher** and repeated
loading **2.6% higher** in this pair. No gameplay improvement is established for
that skin. All p95 results and individual samples are retained in the evidence.

Each cell comes from one before/after process pair. Gameplay runs baseline first;
selectors reverse the order. Cold loading is the median of nine sessions with
fresh preparation services. Repeated loading discards the first of nine samples
and takes the median of eight; this also balances LITONE12's alternating cache
working set. These measurements include session creation and destruction, while
package copying, snapshotting and initial validation are outside the timer.

Frames exclude 256 warmup frames and retain 20,000 samples. Gameplay measures
`prepareFrame` with synthetic chart state and no projected notes. Music select
measures `render`, including CPU submission preparation and a recording backend,
with a 17-song list, sparse properties and a changing selected index. Missing
property fallbacks are common in this fixture and benefit strongly from indexed
catalog admission. These are CPU measurements, not live-app FPS or mobile-device
results: texture/movie devices are fakes, GPU submission and audio are absent.
No builds, tests or other task benchmarks ran during the final comparison.

## Verification

- Full native build and **481/481 CTest tests passed** (124.17 seconds).
- New font and selector allocation regressions failed on the old paths and pass
  after the changes. Existing tests cover changing selector properties, atlas
  updates, interactions, render commands and golden rendering fixtures.
- Numeric catalog parity covers every ID from -4100 through 4100 in all ten
  namespaces, integer boundaries, invalid domains and irrelevant domains.
- Exact canonical font-result hashes match for **63 installed descriptors and
  5,000 generated cases**, including unknown fields, duplicate keys, quoted and
  unterminated values, malformed numbers, supplementary characters and NULs.
- Both selectors retain identical submitted-geometry/render-state fingerprints,
  batch counts and vertex counts across all measured frames. Gameplay command
  counts match. All diagnostic-code sets match; neither gameplay run reports a
  Lua callback-budget violation.
- Independent read-only review found no actionable defects. The combined loading
  and frame benchmark also passes its synthetic smoke check.

Raw samples, build commands, source/binary identities, parity corpus generator
and reproduction scripts are in the [evidence directory](evidence/2026-10-11-skin-performance/README.md).
