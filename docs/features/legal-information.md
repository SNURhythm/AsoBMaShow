# Legal information

Settings → Legal provides an offline selector and a virtualized reader for the application license, third-party copyright/license notices and the translation disclaimer. Notice text is displayed verbatim; navigation is localized. HTTP/HTTPS and `www.` links are underlined and open in the system browser on a completed click or tap; scrolling and dragging cancel link activation. Some components apply only to particular platforms or build configurations. User-supplied charts, music and skins retain their authors’ terms.

## Updating notices

Keep `assets/legal/*.txt` and `src/legal/Notices.inc` in sync when dependencies change. Full texts are packaged through the existing assets pipeline on desktop, Android and iOS. `tests/legal_notices_tests.py` checks catalog completeness, UTF-8 and the application license against root LICENSE.

Sources for the notices added with this page:

- `asobmashow.txt`: `LICENSE`
- `sdl.txt`: `SDL/LICENSE.txt`
- `sdl-ttf.txt`: `SDL_ttf/LICENSE.txt`
- `bgfx.txt`: `bgfx/bgfx/LICENSE`
- `bx.txt`: `bgfx/bx/LICENSE`
- `bimg.txt`: `bgfx/bimg/LICENSE`
- `yoga.txt`: `yoga/LICENSE`
- `pcre2.txt`: `third_party/pcre2/LICENCE.md`
- `libsndfile.txt`: installed vcpkg `share/libsndfile/copyright` (locked project dependencies).
- `libarchive.txt`: installed vcpkg `share/libarchive/copyright` (locked project dependencies).
- `liblzma.txt`: installed vcpkg `share/liblzma/copyright` (locked project dependencies).
- `opus.txt`: installed vcpkg `share/opus/copyright` (locked project dependencies).
- `libvorbis.txt`: installed vcpkg `share/libvorbis/copyright` (locked project dependencies).
- `mpg123.txt`: installed vcpkg `share/mpg123/copyright` (locked project dependencies).
- `libogg.txt`: installed vcpkg `share/libogg/copyright` (locked project dependencies).
- `libflac.txt`: installed vcpkg `share/libflac/copyright` (locked project dependencies).
- `lz4.txt`: installed vcpkg `share/lz4/copyright` (locked project dependencies).
- `c-ares.txt`: installed vcpkg `share/c-ares/copyright` (locked project dependencies).
- `zlib.txt`: installed vcpkg `share/zlib/copyright` (locked project dependencies).
- `lua.txt`: installed vcpkg `share/lua/copyright` (locked project dependencies).
- `curl.txt`: installed vcpkg `share/curl/copyright` (locked project dependencies).
- `mp3lame.txt`: installed vcpkg `share/mp3lame/copyright` (locked project dependencies).
- `zstd.txt`: installed vcpkg `share/zstd/copyright` (locked project dependencies).
- `luajit.txt`: installed vcpkg `share/luajit/copyright` (locked project dependencies).
- `portaudio.txt`: installed vcpkg `share/portaudio/copyright` (locked project dependencies).
- `openssl.txt`: installed vcpkg `share/openssl/copyright` (locked project dependencies).
- `bzip2.txt`: installed vcpkg `share/bzip2/copyright` (locked project dependencies).
- `miniaudio.txt`, `stb-image.txt`, `sol2.txt`: license blocks in the corresponding vendored headers under `include/`.
- `bms-parser.txt`: copyright/license header in `src/bms_parser.hpp` and root GPL text.
- `translation.txt`: Google disclaimer already documented in `docs/features/readme-translation.md`, with provider links.

Existing notices are preserved. This catalog is a maintained set of bundled notices, not an automatically generated complete software bill of materials. Check platform-specific SDK notices when adding or updating SDKs.

The Lua copyright text was converted from Latin-1 to UTF-8 without changing its content.

FreeType notices include `SDL_ttf/external/freetype/LICENSE.TXT`, `docs/FTL.TXT`, `docs/GPLv2.TXT`, and the BDF/PCF README notices. HarfBuzz, PlutoSVG and PlutoVG notices come from their license files under `SDL_ttf/external`. SQLite’s public-domain dedication comes from the opening block of `src/sqlite3.h`.
