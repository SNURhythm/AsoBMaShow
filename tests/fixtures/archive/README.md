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

## 7z compression blocks

`sevenzip_block_fixtures.h` embeds archives generated with 7-Zip 25.01. Each
contains `file0.bin` through `file7.bin`, each 256 KiB. The files contain repeated
`a`, `a`, `b`, `b`, `c`, `c`, `d`, and `d` bytes, respectively, plus an empty
`empty.bin` file and `emptydir` directory. Expanded file data totals 2 MiB.

Common creation options are `7zz a -t7z -mmt=1 -mtm=off -mta=off -mtc=off
-mhc=off`, followed by these options, the destination, `file0.bin` through
`file7.bin`, `empty.bin`, and `emptydir`:

| Fixture | Additional options | Blocks | Bytes |
| --- | --- | ---: | ---: |
| `blocksLzma` | `-m0=LZMA:d=1m -ms=2f` | 4 | 1,014 |
| `blocksLzma2` | `-m0=LZMA2:d=1m -ms=2f` | 4 | 1,010 |
| `solidLzma2` | `-m0=LZMA2:d=1m -ms=on` | 1 | 786 |
| `filteredBlocks` | `-m0=Delta:4 -m1=LZMA2:d=1m -mb0:1 -ms=2f` | 4 | 1,054 |

`compressedHeader` uses the `blocksLzma2` options but omits `-mhc=off` (826
bytes). Its regression test checks that all private handlers are prepared before
the first file-extraction progress callback, so transient compressed-header
decoders do not run concurrently with each other or with data-block decoders.

Dictionary-admission tests change the four LZMA2 property bytes in the uncompressed
header to advertise 3 MiB or 64 MiB dictionaries, then recalculate both header
CRCs. The unchanged payload remains decodable with those larger dictionaries.
The corruption test instead changes packed payload byte 50 without changing its
checksum. Runtime tests need neither an encoder nor large fixture files.
