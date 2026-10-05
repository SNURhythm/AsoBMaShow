# Skin archive fixtures

Both archives contain authored test data only:

- `Wrapper/play.luaskin`: `return {type = 0}` followed by a newline.
- `Wrapper/assets/image.txt`: `asset bytes`.

`minimal-rar5.rar` was created with `rar a -ma5` from this tree.
`minimal-lh0.lzh` uses LHA level-0 stored (`-lh0-`) entries with
IBM CRC-16 checksums. Both can be checked with `7zz t`.
