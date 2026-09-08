# Find BMS extraction limits

Find BMS applies these limits to each downloaded archive that must be unpacked:

- Maximum expanded member: 2 GiB.
- Maximum total expansion: 8 GiB.
- Maximum directory entries: 100,000, including directories and skipped entries.
- Keep at least 256 MiB of the initially available staging space in reserve.

These ceilings allow large chart packages without allowing one small compressed
download to consume unlimited storage. A package above a ceiling must be handled
manually; the failure message identifies the exceeded limit. Limits apply per
archive, not to an entire library. The existing option to retain supported
archives without unpacking is unchanged.

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
