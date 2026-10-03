# Historical PMS Scan fixture

`pms_scan_baseline.h` records output actually captured from the immutable
AsoBMaShow parser pair at `68382de1627f321aa8a56c7a961d75b4f7b974b7`.
Source identities:

- header: `ce853b35c45f27b2fde0f3c2264ccdef0f6e01c148b005997837035cdb505c42`
- implementation: `ad33611897caa887c3e82dbb8edd6602ae40425647334d257acca0eda6c7433f`

The capture compiled those files with Clang C++23 `-O1`, wrote `chart` verbatim
to `mapping.pms`, set `Parser::SetRandomSeed(1)`, then called
`Scan(path, cancelled)` and `Scan(bytes, cancelled)` on the identical bytes.
The latter reproduces the parser call used for archive-entry buffers. The old
scanner discovery allowlist excluded PMS, so these are historical parser facts,
not evidence of PMS rows created by the unmodified historical scanner:

```text
ordinary-path keys=9 dp=0 notes=4 longs=1 scratches=0 spins=0 mines=0 playLength=3000000 totalLength=4000000 sha256=ab3ec05940186d15534825fadef2a560e08e8d16fa7c24dbe7fe1af22eb52b82
archive-buffer keys=10 dp=1 notes=4 longs=1 scratches=0 spins=0 mines=0 playLength=3000000 totalLength=4000000 sha256=ab3ec05940186d15534825fadef2a560e08e8d16fa7c24dbe7fe1af22eb52b82
```

`testHistoricalPmsMetadataRebuildsUnchangedSources` seeds schema-12 rows with
these captured persistent facts, preserving a populated archive scan cache.
Those PMS rows are seeded explicitly because historical discovery excluded PMS.
After upgrade, unchanged ordinary/archived sources both produce 9K metadata;
the archived result therefore changes despite an unchanged chart hash and
archive size/time. Ordinary metadata retains its already-correct 9K mapping.
Added dates survive and a second scan reuses the rebuilt cache. This is captured
historical parser output seeded into the database, not a claim to have executed
the entire historical scanner/repository binary.

`testPmsFormatSurvivesBufferedChartLoading` additionally verifies actual lanes,
reciprocal LN links, counts, ordinary BMS mapping, extension casing, archive
inner names, and buffered Android SAF logical paths. Real Android provider I/O
is outside this desktop test; the source hint carries the logical filename.
