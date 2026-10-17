# Archive storage updates

## Corrected requirement

Storage management must replace media inside existing archives, batching selected
folders that belong to the same physical archive. The result stays at the same
archive path and in the same format. A second complete archive or a complete
extracted tree is not acceptable. This supersedes the archive exclusion in the
original storage-management design.

Existing audio/video policies, collision rules, chart identity preservation,
selected-folder boundaries, iOS bookmark access, and maintenance admission still
apply. Review counts eligible archive entries and their logical input bytes.
Reclaimed bytes for archives are the actual reduction of the container file.

## Format capability

Reading a format does not imply that it can be updated under this constraint.
Implement format-specific update adapters for ordinary single-volume ZIP/ZIP64
(stored and deflated regular entries) and uncompressed regular-file tar archives.
Preserve untouched entry contents and names. Reject encryption, ambiguous names,
overlapping records, links/special entries, unsupported metadata extensions and
container variants before mutation. Other formats remain visible, with an
explicit unsupported-in-place-update reason; there is no full-copy fallback.

## Bounded storage and batching

Use a private SQLite manifest with a bounded page cache rather than retaining
every archive entry or conversion in RAM. Review stores compact per-archive
evidence and selected inner roots. Rebuild and verify the manifest when applying.

Process one archive at a time. Extract and encode one candidate at a time, with a
scratch budget; do not materialize the whole archive. A replacement must fit in
the old entry's physical slot. Already compressed output can be stored directly
in ZIP. A replacement that does not reduce its container record is skipped.

Walk records once in physical order, installing each verified replacement at
the current output cursor and omitting deleted records. The new record must fit
within the original extent, so compaction never overwrites a future input. Copy
only bounded chunks toward lower offsets, update container metadata once, then
truncate. Untouched payloads are copied as stored bytes, without recompression.
Metadata and a bounded recovery journal are additional to per-candidate scratch;
no second archive-sized file is created.

## Durability and recovery

An in-place update cannot keep a complete original while also avoiding its
backup. It uses durable forward recovery, not whole-archive rollback. Before
overwriting bytes, persist the desired bytes and the operation's progress in a
private journal. Sync the archive before advancing the durable cursor. Replay a
pending write idempotently after interruption. Keep prepared replacement data
until its writes and corresponding manifest changes are durable.

Before mutation, persist the original manifest and hashes of source chunks. A chunk
must match its planned hash before it can be moved. On recovery, validate already
written output and remaining source against that evidence. A private preparation
directory becomes a recovery journal only after its initial index is durable.
Truncate only after every
planned write has completed. Detect unauthorized paths, changed file identity,
links, corrupt journals and missing recovery data; retain the maintenance block
when safe recovery cannot finish.

Startup recovery runs before readers and cache users reopen archives. Cancellation
stops new conversions, then finalizes the already committed entries into a valid
archive. Once a durable in-place write starts, its recovery/finalization cannot
be abandoned halfway. Previously completed conversions remain changed, matching
the existing cancellation contract.

## Integration

Reserve the whole physical archive, even for one selected inner folder. Coalesce
overlapping selections, and scan the archive once for the union. Revalidate the
archive identity and selection/policy evidence at admission and before mutation.
Refresh the physical archive once after completion, invalidating measurements
for all its displayed groups. Other inner folders are preserved byte-for-byte.

The review stays aggregated: eligible entries/bytes, skipped entries, and counts
of excluded folders by reason. Unsupported containers must not be presented as
having no media. The archive explanation added during diagnosis will become a
format-capability explanation rather than requiring every archive to be extracted.

## Verification

Use small real archives, read their results through the existing archive reader,
and compare unselected/chart/image payload hashes. Cover stored/deflated ZIP,
ZIP64 offsets, comments and Unicode/CP932 names, tar, overlapping folder selection,
same-stem conflicts, corrupt/encrypted/unsupported containers, tight scratch
space, cancellation and failed conversions. Inject interruption before and after
durable writes and truncation; reopen the journal and finish recovery. Measure
scratch high-water use and ensure one compaction per physical archive.

Run focused storage/archive/localization tests, the desktop build, and an unsigned
iOS build. Commit and push verified work to the current upstream; do not deploy.
