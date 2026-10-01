# simdutf 9.2.1

Source: https://github.com/simdutf/simdutf/tree/v9.2.1

Vendored under the MIT license in `../../../assets/legal/simdutf.txt`.
Release archive SHA-256:
`582f9d0dcf578f6d4766fa29ea12a7f2f02bd3c6ad9e0cf35a8e0ec8478eba4b`.

Generated from the verified release archive with:

```sh
python3 singleheader/amalgamate.py --with-utf8 --with-utf32 --no-zip --no-readme --output-dir /tmp/simdutf
```

Copy `simdutf.h` and rename `simdutf.cpp` to `simdutf.cpp.inc` here.
The first generated comment contains a timestamp when built from a tarball;
all subsequent lines are reproducible. Do not edit the generated code.
Only `../Utf8.cpp` includes it, so Xcode and CMake compile it exactly once.
CPU dispatch and the portable fallback are retained; UTF-16, ASCII-specific,
Latin-1, Base64 and encoding-detection features are omitted.

Checked-in SHA-256:

- `simdutf.h`: `423d37dac5802169f82fdd3491416c1895bfdf855f1797fcd750a3b0e457a289`
- `simdutf.cpp.inc`: `55332e4f03babaae196cade0573a4efebea5f8bd6018d69b1bb5ff42b2be4742`
