# Find BMS archive limits

## Chart verification

Packed and extracted BMS verification has separate, smaller limits:

- Maximum BMS member: 16 MiB.
- Maximum cumulative BMS verification bytes per attempt: 256 MiB.
- Maximum inspected entries: 100,000, including directories and skipped entries.
- Read/decode and incremental SHA-256/MD5 checkpoints: 64 KiB.

Declared BMS demand is admitted before reading any member. Actual bytes are
bounded separately while reading. Only one BMS payload is retained at a time;
MD5 no longer copies it into an equally sized string. Hashless packages still
confirm that their BMS files are readable. A matching earlier member does not
bypass admission or read failures in later members. Empty charts and packages
without BMS files retain the existing hash-match/hashless distinctions.

Cancellation reaches listing, bounded readers and chunked hashing directly,
without a monitor thread, and is checked again after extracted verification
before publication. A verification budget, cancellation or read-integrity
failure is a download failure, never a pending Keep Files choice. Normal hash
mismatches retain that choice. Solid archives and unavailable direct codecs
can use the bounded extraction path; completed packed verification bytes are
deducted from its remaining verification budget. Unsupported ZIP codecs may
also use the existing bounded libarchive reader without unpacking.

These are chart verification limits, not audio/BGA limits or whole-library
limits. Over-budget packages must be handled manually. Trusted fixtures may
inject smaller `ArchiveVerificationLimits`; production callers use the stated
defaults.

This is not an archive-index or process-wide memory ceiling. The existing
decoder metadata, central directory, cached indexes, path lists and codec
workspace still have input-dependent costs. Bounded listing counts raw ZIP and
7-Zip items before application entry reservation, or libarchive headers before
filtering; decoder open/index work can precede that check. The existing cached
index may subsequently be built by a bounded member read. Vector growth can
temporarily retain both old and new allocations, but member capacities remain
bounded. Cancellation must wait for the current filesystem/decoder operation
to return; publication is not an atomic transaction with a concurrent cancel.

## Extraction storage

Find BMS applies these limits to each downloaded archive that must be unpacked:

- Maximum expanded member: 2 GiB.
- Maximum total expansion: 8 GiB.
- Maximum directory entries: 100,000, including directories and skipped entries.
- Keep at least 256 MiB of the initially available staging space in reserve.

These ceilings allow large chart packages without allowing one small compressed
download to consume unlimited storage. A package above a ceiling must be handled
manually; the failure message identifies the exceeded limit. Limits apply per
archive, not to an entire library. The option to retain supported archives
without unpacking is subject to the chart verification limits above.

Declared sizes are checked before opening member outputs. Actual decoded bytes
are checked separately before every write, including archives with missing or
false size metadata. The miniz ZIP path streams through bounded callbacks; the
libarchive path reads bounded chunks, including when discarding unsafe or
unsupported members. Cancellation is checked at entry and chunk boundaries.
The underlying decoder must still return from its current read before a stop
can be observed.

The free-space check is a snapshot, not a reservation against concurrent writers.
Output errors, including final close errors, fail extraction. The download
workflow removes its private attempt directory on failure or cancellation;
the extraction backends do not recursively delete caller-owned output paths.

`ArchiveExtractionLimits` permits trusted callers and regression fixtures to use
smaller budgets. Tests use small compressed files, deliberately incorrect size
metadata, both enabled backends, and a real owned-attempt cleanup path rather
than trying to fill the disk.
