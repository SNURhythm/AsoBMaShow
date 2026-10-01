# Full static UI fonts

App UI and CJK chart text use one full Noto Sans CJK JP Regular face and one
matching Bold face. These are the full language-specific packages, not the
regional packages or a subset derived from application text. All 65,535 glyphs
remain in each file. The separate Japanese and Korean regular assets are removed.

`JP` selects Japanese default shapes for shared Han characters. It does not
limit Unicode coverage to Japanese. Both faces include Korean Hangul, kana,
Chinese ideographs, Latin, and other symbols. See the upstream
[language-specific OTF documentation](https://github.com/notofonts/noto-cjk/blob/main/Sans/README.md#language-specific-otfs).

| Asset | Bytes | Mapped Unicode characters |
| --- | ---: | ---: |
| `assets/fonts/notosanscjkjp.ttf` | 16,467,712 | 44,811 |
| `assets/fonts/notosanscjkjp-bold.otf` | 17,032,596 | 44,811 |

The pair occupies 31.95 MiB. The regular asset retains its historical filename
for existing application references; its data is OpenType CFF, as before.
`TextView::FontWeight::Bold` selects the native bold face, including for older
bundled Noto path aliases. SDL_ttf recognizes that face's native bold flag and
does not synthesize thicker pixels. Custom fonts, icons, and generic system
fallback behavior remain available for scripts outside this CJK font family.

## Source and license

Upstream: https://github.com/notofonts/noto-cjk

Pinned revision: `f8d157532fbfaeda587e826d4cd5b21a49186f7c`

| Source path | Upstream SHA-256 |
| --- | --- |
| `Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf` | `68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5` |
| `Sans/OTF/Japanese/NotoSansCJKjp-Bold.otf` | `e53dcb0dcb2922e45d01aae1ebd2f382bb81d4229b18b6b883bd170678af1f76` |

The SIL OFL 1.1 license and copyright notice ship in
`assets/legal/noto-sans-cjk.txt`.

## Legacy coverage compatibility

Upstream deliberately leaves U+2252 `≒` unmapped for its Noto fallback policy,
although its original Source Han glyph remains CID+858. See
[Noto issue 140](https://github.com/notofonts/noto-cjk/issues/140).
The old primary UI font mapped that character, so our preparation script
restores its Unicode cmap entry in both weights. No outlines are changed or
removed. The resulting fonts preserve all 28,926 mapped characters from the
old regular stack and add 15,885 more. Their hashes are:

- Regular: `7ae5f8546575719853db6bc20d6cbaf61d8fdfc90d68204bc24f7f5e5e317636`
- Bold: `d55a53960d3e026f22533ff796e524bf5a91fedd2b3d1f45f5c5642e182509d5`

To regenerate, install `fonttools==4.61.1` in a development virtual environment,
download the two source files above at the pinned revision into one directory,
then run:

```sh
python scripts/prepare_ui_fonts.py --source-dir /path/to/upstream-fonts
```

The script verifies input hashes and retains all glyphs. FontTools is only a
development tool; application builds consume the committed fonts directly.

## Verification

`tests/fixtures/ui_font_legacy_coverage.txt` records the old regular fonts'
Unicode coverage and hashes. The native font test checks every legacy character,
compares the complete Unicode coverage of Regular and Bold, verifies cache
sharing and cleanup, and compares rendered English/Korean/Japanese text against
the directly opened static faces. Representative CJK characters outside the UI
catalog must render from the primary font, without fallback. Reminder layout
checks use the actual fonts in all three interface languages.
