# Incremental Archive Chart Count Design

## Problem

Archive indexing already produces an ordered list of candidate chart entries. After parsing, the scanner nevertheless calls `CountChartsInArchive` once per completed archive before writing its cache record. That repository method selects every `chart_meta.path` row and performs filesystem path comparisons in C++.

The exported iOS trace shows this repeated full-table scan growing from 182 ms to 562 ms as the database fills. After the final archive stream completed, inter-batch gaps and the last recount consumed about 5.1 seconds, compared with 1.684 seconds of actual inserts.

The candidate count cannot be cached directly because parsing can reject entries without notes or stable identities, and storage can fail. The cache needs the number of valid chart rows successfully stored for the archive.

## Design

For a normal reindex, existing chart rows for the archive are deleted before parsing. The scanner will therefore initialize the archive's stored-chart count to zero and increment it only when `UpsertChart` succeeds. After a complete batch, it will pass that accumulated count directly to the archive-cache writer.

When resuming inside an archive (`innerStart > 0`), the earlier valid rows were committed by a checkpoint and protected from deletion. The scanner will call `CountChartsInArchive` once to initialize the accumulated count, then add successful inserts from the remaining suffix. Resuming at an archive boundary still starts from zero and performs no count query.

`CountChartsInArchive` remains available solely for the mid-archive recovery path. No schema, archive scheduling, parsing, cache acceptance, or checkpoint-frequency changes are included.

## Correctness

- Candidate paths continue to determine scheduling and progress totals.
- Only successful `UpsertChart` operations contribute to the cached valid-chart count.
- Invalid or uninsertable candidates do not inflate the cache and therefore do not force future rescans.
- Incomplete or cancelled archive batches still skip the cache write.
- A resumed archive combines its previously committed valid-row count with successful suffix inserts.

## Tests

- A real multi-archive scanner test will use SQLite authorization callbacks to assert that a normal scan does not read `chart_meta.path` after chart insertion begins. The existing implementation fails because each cache write performs a recount.
- A real archive checkpoint-resume test will stop after 100 candidates, including one invalid candidate, then resume and verify that the database and cache both contain the exact valid count.
- Existing scanner, scheduler, archive-concurrency, document-handoff, and desktop build checks remain required.

## Expected Effect

The change removes the per-archive `archives × database rows` hot path. Based on the supplied trace, it should remove at least the approximately 5.1-second recount tail after streaming, plus any earlier recount work not hidden by archive parsing.
