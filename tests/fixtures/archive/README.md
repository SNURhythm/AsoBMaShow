# Archive fixtures

`rar_fixtures.h` embeds small, portable archives so regression tests do not require
a RAR encoder at runtime.

- `nonSolid` and `solid` were generated with RAR 7.22. Each contains four files,
  `file0.bin` through `file3.bin`, containing 1 MiB of `a`, `b`, `c`, and `d`
  respectively. Creation options were `a -ma5 -m3 -md1m -ts- -s-` and
  `a -ma5 -m3 -md1m -ts- -s`. Both archives are 425 bytes.
- `rar4` is the 336-byte decoded fixture from
  [libarchive v3.8.5, test_read_format_rar.rar.uu](https://github.com/libarchive/libarchive/blob/v3.8.5/libarchive/test/test_read_format_rar.rar.uu).
  Its upstream project licensing is recorded in
  [libarchive COPYING](https://github.com/libarchive/libarchive/blob/v3.8.5/COPYING).
  It contains two copies of `test text document\r\n`, a `testlink` entry whose
  payload is `test.txt`, and directory entries. Extraction tests verify the
  existing regular-file treatment of the link payload, not symlink creation.

The tests derive corrupt-payload, duplicate-name, and large-dictionary variants
from the RAR5 fixture. Changed header CRCs are recalculated; the corrupt-payload
case intentionally retains the original payload checksum. A separate test edits
the persisted RAR4 index to verify that a different SDK path interpretation
declines the concurrent route before writing files.
The RAR4 mixed-version variant changes one stored entry's unpack version and
recalculates its header CRC, checking conservative serialization without needing
to allocate multiple large PPM dictionaries during the test.
