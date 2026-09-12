# Mixed-encryption 7z fixture

`mixed_encryption_sevenzip.h` embeds a genuine 267-byte 7z created with 7-Zip
25.01. It contains an unencrypted `chart.bms` and AES-encrypted `secret.txt`
in separate non-solid blocks, with unencrypted headers. Tests do not require
7-Zip to be installed or a password to be supplied.

Input `chart.bms` (LF endings, final newline):

```text
#TITLE Mixed encryption
#BPM 120
#00111:01
```

Input `secret.txt`: `encrypted-only copy` followed by a newline.

Generation commands, executed beside those two input files:

```sh
7zz a mixed.7z chart.bms -mhe=off -ms=off -mtm=off -mta=off -mtc=off
7zz a mixed.7z secret.txt -preview-only -mhe=off -ms=off -mtm=off -mta=off -mtc=off
7zz l -slt mixed.7z
```

The second command's `-p` option supplies the test password `review-only`.
Listing confirms `Encrypted = -` for `chart.bms` (43 bytes) and
`Encrypted = +` for `secret.txt` (20 bytes), with two compression blocks.
Encryption randomness means regenerating may produce different archive bytes.
