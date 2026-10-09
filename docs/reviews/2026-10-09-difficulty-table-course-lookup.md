# Difficulty-table course lookup

Opening a difficulty table in music select loads its level list and course
stages synchronously. The course-stage fallback query used a combined SHA-256
or MD5 predicate followed by authored sort order and title. SQLite chose the
table's sort-order index, scanning unrelated entries for every course stage.

The query now collects candidates separately through the existing
`(table_id, sha256)` and `(table_id, md5)` indexes, unions them, and applies the
same ordering. Table loading remains synchronous. No schema changes or new
indexes are required.

## Measurements

Read-only SQLite queries against a temporary snapshot of the local library
(69,151 charts, 13,561 table entries, 222 courses) took 511 ms before and 39 ms
after for all course queries. All 890 complete result rows were identical.
The query plan changed from `idx_difficulty_entries_table_sort_order` to the
two table/hash indexes.

A temporary MSVC Release harness exercised the production metadata loader
and course projection against database copies. With SQL tracing enabled in
both runs, the largest table's metadata loading fell from 298 ms to 68 ms;
course score/replay projection took 35 ms before and 27 ms after. These are
single process-pair observations, not a reproduction of the reported full
three-second UI freeze. The profiling harness and local paths were removed.

## Regression

`chart_repository_tests --course-lookup-test` creates a temporary synthetic
database with 10,000 unrelated entries. It verifies table isolation, SHA-256
and MD5 fallback, authored ordering, empty hashes, and missing chart rows.
SQLite VM steps fell from 431,670 to 1,683. The test requires fewer than 5,000
steps to catch a return to scanning the whole table without relying on timing.
The regression also runs in the full chart repository suite.

## Verification

MSVC Release builds of `main`, `chart_repository_tests`, and
`chart_selector_query_tests` passed. The focused `--course-lookup-test` run
also executes the existing chart-query behavior matrix and difficulty-entry
download/fallback tests; all passed. The selector-query CTest suite passed.

The full `chart_repository_tests` run stops in its existing search-duration
fallback coverage with `The library changed while loading this folder.`
The `--search-paging-tests` run reproduced the same failure with the original
production query restored. That unrelated failure remains unresolved here.
