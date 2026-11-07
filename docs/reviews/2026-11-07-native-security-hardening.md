# Native security hardening — 2026-11-07

This update closes the confirmed Lua skin native-access boundary and refreshes
native dependencies with upstream fixes. It uses narrow vcpkg overlays rather
than moving the entire dependency baseline. No deployment is part of this work.

## Dependency provenance and applicability

| Component | Previous | Selected source | Reason and app exposure |
| --- | --- | --- | --- |
| LuaJIT | `3c4f9fe2052b8d08a917ac0d5f38563f0297b5a3` | Same source, FFI disabled at build time | Skin loaders now accept source only. Removing globals alone did not enforce the native-access boundary. |
| libsndfile | 1.2.2 | Upstream `396a715daeae7316f047ee220b364ab983b66df4` (2026-10-28), overlay revision 4 | Includes the IMA ADPCM frame-count overflow fixes associated with CVE-2026-37555. Chart and skin audio reach this decoder. The app already rejected negative frame counts; an app-level buffer overwrite was not established. |
| curl | 8.18.0 | 8.23.0 | Refreshes HTTP/TLS client fixes; individual advisory prerequisites still depend on enabled features and options. |
| OpenSSL | 3.6.0 | 3.6.5 | Remains on the same release series and includes the September certificate-processing fixes. Used on applicable desktop/Android builds, not iOS. |
| libarchive | 3.8.5 | 3.8.11 | Security and bugfix maintenance release for archive fallback readers. ZIP/RAR may use other backends, so not every libarchive advisory describes the preferred path. |
| miniaudio | 0.11.21 plus app extensions | 0.11.26 plus retained app extensions | Upstream addresses asynchronous resource lifetime races, duplex buffer overflow, and AAudio rerouting issues. These are upstream fixes, not newly proven app exploits. |
| stb_image | 2.30 | Upstream dev commit `3f9200aae6a557d281cd9ce53a7c5059706d9871` (2026-11-03) | Full upstream header includes PNG validation, JPEG initialization/bounds and GIF decoder fixes; no selected security backports. Used for chart and skin imagery. |
| PCRE2 | 10.47 | 10.49, `6f9d7c1373262c541324a16a358785b33ef116cf` | Includes the 10.48 security fixes and follow-up corrections. The app disables PCRE2 JIT; JIT-only advisories are not applicable, but skins can supply regex patterns. |
| miniz | 3.0.0 from bimg | Official 3.1.2 amalgamation | Includes ZIP central-directory offset overflow and decompression progress fixes. App-owned archive, replay gzip, PNG and export paths now use `third_party/miniz`; bimg itself is unchanged. |
| SQLite | 3.53.4 | Unchanged | Already contains the fixes for CVE-2026-11822/11824 and older listed CVEs. The 3.54.0-only 32-bit overflow does not affect 3.53.4. |
| FreeType (SDL_ttf) | SDL fork of 2.13.3, `535d2993d` | Complete upstream `5a280ecde6f324de0d226261036e736e0cb49a71` (2.14.3 plus fixes, 2026-05-13) | Replaces unchecked COLR bitmap allocation arithmetic associated with CVE-2026-49919. Includes the variable-coordinate fix for CVE-2026-50811, without importing selected security patches. |

The libsndfile project has not released a newer tagged version containing the
ADPCM fixes, so its complete upstream snapshot is pinned rather than carrying
manual security backports. The remaining libsndfile patch only adjusts vcpkg
package installation. Every downloaded vcpkg source has a pinned SHA-512.

The miniaudio refresh retains native CoreAudio output-boundary timing and the
app's AAudio burst-size negotiation, exclusive/shared fallback, and reroute
settings. The source delta against upstream is limited to these existing app
extensions. SQLite's source ID is
`2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc`.

Other embedded sources checked include nlohmann/json 3.11.3 and
stb_image_resize2 2.18. No additional applicable published advisory was confirmed
for those in this review. This is not a claim that all vendored code is free of
memory-safety defects.

## Lua compatibility with Beatoraja

Compared with local Beatoraja commit
`82365b8cd8d54f401bc9ea9f4b9e82d105ef7f92`, particularly
`SkinLuaAccessor.java` and its standard/sandbox global setup:

- Beatoraja uses LuaJ on the JVM, without LuaJIT FFI. Compiling out FFI therefore
  removes a VM-specific extension, not a Beatoraja skin API.
- Beatoraja's `execFile` accepts source and LuaJ-compatible binary chunks (`bt`).
  LuaJ bytecode is incompatible with LuaJIT bytecode. AsoBMaShow intentionally
  accepts source only in every safety mode, including unrestricted mode.
- Source `load`, `loadstring`, `loadfile`, `dofile`, `require`, entry files,
  property scripts and binding scripts retain their existing host behavior.
  Each native compiler entry point explicitly requests text mode (`t`).
- LuaJIT-only integer/complex numeric suffixes (`LL`, `ULL`, `i`) are unavailable
  when FFI is compiled out. Ordinary Lua numbers and hexadecimal literals remain
  supported. No source rewrite or change to the existing numeric conversions
  was introduced.

The FFI setting applies to target libraries and build generators, including
Windows assembly generation. iOS builds its generators on macOS with iOS VM
flags; it must not reuse a generic host generator configured with JIT enabled.

## Image compatibility adjustments

The row decoders follow the updated upstream GIF background/transparency, BMP
BI_RGB opacity and big-endian PNM behavior. Binary P5/P6 images use the existing
bounded row decoder at every output size, with inclusive MAXVAL normalization;
this avoids an upstream off-by-one that turns MAXVAL=1 images entirely black.
TGA's upstream decoder now
honors horizontal origin, so the test reference no longer applies a second
reflection. The app's existing bounded PIC row decoder handles both full-size
and reduced images after upstream removed PIC support. JPEG's component-row
path still builds against the updated header without an architectural change.

The selected stb snapshot newly supports ASCII P2/P3. Those formats still use
its upstream decoder and retain the upstream nonstandard-MAXVAL edge case;
the compatibility correction covers the previously supported binary formats.

AAudio tests distinguish upstream's new device-selected default buffer capacity
from explicit capacity requests and the app's hardware-burst policy. Archive
tests retain allocation/output bounds while accepting libarchive's earlier
rejection of understated ZIP sizes. Global PAX tests use an informational
comment and verify rejection of unsupported global member attributes instead
of relying on the older decoder silently ignoring them.

## iOS artifacts

`scripts/get_ios_libs.py --packages libsndfile libarchive luajit` refreshes
the affected checked-in device and simulator XCFrameworks and headers. Classic
vcpkg install otherwise considers an already installed package current even
when its overlay changed; the helper removes selected packages before resolving
their current ABI. Matching binary caches are still reusable. Framework copies
replace the prior directory so renamed slices cannot leave stale libraries.

iOS networking uses the Apple implementation and does not link curl/OpenSSL.
The miniaudio/stb/miniz sources and PCRE2 submodule are compiled into iOS directly.

## Follow-up review: fonts and media input

**High — COLR bitmap allocation overflow.** The previous vendored FreeType
multiplied combined layer dimensions in 32-bit unsigned arithmetic before
allocating the destination bitmap. The new regression supplies synthetic glyph
bounds for `16384 * 65537 * 4` bytes and an allocator that refuses all requests.
The old code requested only 65,536 bytes; the selected upstream code returns
`FT_Err_Array_Too_Large` before calling the allocator. No oversized payload or
unsafe copy is needed for this check. Skin fonts reach FreeType through SDL_ttf's
color-glyph loading; an end-to-end application exploit was not attempted.
The upstream allocation correction is linked from the
[Android advisory](https://source.android.com/docs/security/bulletin/2026/2026-09-01)
as [the COLR fix](https://android.googlesource.com/platform/external/freetype/+/65f3c6ba20e777f45f409ae2a7500c4583a6545f).

The source snapshot is unchanged upstream. SDL_ttf retains its static symbol
visibility through a configuration-header wrapper and uses its existing HarfBuzz
target, with dynamic HarfBuzz loading and the new optional Apple HVF backend
disabled. The Xcode source list includes
the new HVF module stub; the former SDL Android makefile lives in SDL_ttf's build
support rather than modifying the upstream font sources. iOS builds FreeType
from this submodule as part of `SDL3_ttf.framework`; it is not a separate
checked-in XCFramework.

**Medium — media could open secondary files.** A harmless ffconcat fixture
named with a video extension caused the storage decoder to open another local
file. The shared input helper now opens the selected path literally, supplies
its I/O context, and denies additional I/O. An empty protocol whitelist also
protects child contexts that do not inherit the callback. Device/sequence
demuxers and playlists are excluded before header parsing. BGA playback,
transcoding, FLAC validation, and memory-backed WebP decoding use this policy;
FLAC and WebP additionally select their expected demuxer. Ordinary single-file
media, literal path punctuation, and cleanup after rejection have regression
coverage. The local fixture confirms secondary-file access; it does not claim
that network access or data exfiltration was demonstrated.

**Medium — special-file inputs could block media loading.** A chart's media path
could name a FIFO, blocking inside the file open before probing or cancellation.
The input helper now opens with nonblocking flags on POSIX, requires a regular
file through descriptor inspection, and passes that same descriptor to FFmpeg.
It never reopens the checked pathname. A five-second bounded test reproduced
the old hang and now passes immediately. Regular `/proc/self/fd`-style inputs
remain supported; the host regression also checks ownership after the original
descriptor closes.

Additional advisory checks:

- FFmpeg 8.1.2 retains the TY packet-size underflow corrected by
  [this upstream change](https://github.com/FFmpeg/FFmpeg/commit/de771bd52774a52d45b0e2c82e56995a1ef40df7)
  (CVE-2026-65704). The published concat/Shorten route needs demuxer options the
  app did not enable. The new input policy also excludes concat entirely.
  This is an application-side restriction, not an FFmpeg source update or a
  claim that every decoder in the library is free of defects.
- The old SDL FreeType fork already contained CVE-2026-23865's upstream fix,
  despite retaining its 2.13.3 version number. The selected newer snapshot also
  contains it and the
  [variable-coordinate bounds fix](https://github.com/freetype/freetype/commit/5a280ecde6f324de0d226261036e736e0cb49a71).
- [HarfBuzz GHSA-q4gc-p4hh-3765](https://github.com/harfbuzz/harfbuzz/security/advisories/GHSA-q4gc-p4hh-3765)
  requires an out-of-range text offset. SDL_ttf supplies offset zero and length
  `-1`; this call path does not meet the advisory prerequisites.
- The reviewed 7-Zip XZ and filesystem-image advisories concern handlers absent
  from the app's explicitly selected ZIP/RAR/RAR5/7z/LZH backend list. No direct
  affected-handler path was identified. The app does not use 7-Zip's CLI
  extraction or Mark-of-the-Web propagation.
- FreeType's optional PNG backend is disabled in the reviewed build. Version
  matches for the unused vcpkg FreeType/libpng packages do not describe the
  SDL_ttf font implementation actually linked by the app.

## Verification

Regression tests cover valid, harmless LuaJIT binary chunks at entry, dynamic,
file, module and callback boundaries in all three skin safety modes. A virtual
1.28 GB WAV fixture checks the 2,525,000,000-frame ADPCM calculation while reading
only a bounded amount of data. The image suite checks PNG metadata rejection
and native/reduced-format compatibility with small synthetic fixtures.

Final verification:

- Desktop `main` and the eight affected subsystem groups built successfully.
- All 265 selected CTest checks passed at `-j 6` (35.66 seconds): audio,
  skin runtime, rendering, archives, downloads, IR, replay and results.
- All 56 iOS build-setup and 20 iOS release-workflow Python checks passed.
- Device and simulator XCFrameworks were regenerated. Both LuaJIT slices
  export the text-mode loader and omit FFI symbols.
- `scripts/ios_firebase_deploy.sh --build-only` and the incremental final-source
  build passed; the unsigned iOS app passed `scripts/ios_artifact_audit.sh`.
- Android audio source passed a syntax check with NDK 28.2.13676358; the host
  test of the real AAudio backend also passed. A full Android APK build was not
  run.
- Installed OpenSSL headers and the linked `libcrypto.a` identify 3.6.5,
  independently of stale private pkg-config version entries in CMake's cache.

The initial regression run reproduced binary Lua acceptance and overflowing
ADPCM frame counts before their fixes. The binary PNM correction also had a
failing endpoint/reduction regression before the inclusive-MAXVAL fix. No
distribution action was performed.

Follow-up verification after the font and media changes:

- Desktop `main`, `tests_rendering`, and `tests_video` built successfully.
- All 77 rendering/video CTest checks passed at `-j 6` (13.31 seconds), including
  the COLR allocation, secondary-file access, FIFO, and descriptor-lifetime tests.
- The final unsigned iOS build and artifact audit passed.
- Symbol inspection confirmed that the desktop FreeType archive and iOS
  FreeType objects mark their public APIs as private external symbols.
- Both iOS FFmpeg configurations include the descriptor protocol used by the
  input helper. Full Android and Windows builds were not run.

## Review limits and follow-up

The initial independent review found no remaining security or
application-integration blocker after the binary PNM correction. The follow-up review identified the
FIFO input issue and an ineffective FreeType visibility definition; both were
corrected and verified with the checks above. One packaging-only follow-up remains:
the libsndfile overlay does not carry the baseline `mp3lame-static.diff`, so its
Windows static pkg-config metadata still names `mp3lame` rather than the static
library names. The app consumes the CMake `SndFile::sndfile` target, not that
pkg-config metadata; no affected application build path was identified.

This was a focused integration and advisory review, not an exhaustive audit of
the imported upstream implementations. Real-device audio behavior and Windows
execution were not verified in this task.

## Sources

- [libsndfile ADPCM fix](https://github.com/libsndfile/libsndfile/pull/1123)
  and [selected upstream source](https://github.com/libsndfile/libsndfile/commit/396a715daeae7316f047ee220b364ab983b66df4).
- [curl 8.18.0 advisory table](https://curl.se/docs/vuln-8.18.0.html).
- [OpenSSL 3.6 advisories](https://openssl-library.org/news/vulnerabilities-3.6/).
- [libarchive 3.8.11 release](https://github.com/libarchive/libarchive/releases/tag/v3.8.11).
- [miniaudio 0.11.26 release](https://github.com/mackron/miniaudio/releases/tag/0.11.26).
- [stb_image pinned source](https://github.com/nothings/stb/blob/3f9200aae6a557d281cd9ce53a7c5059706d9871/stb_image.h).
- [PCRE2 security policy and advisories](https://github.com/PCRE2Project/pcre2/security).
- [miniz 3.1.2 release](https://github.com/richgel999/miniz/releases/tag/3.1.2)
  and [upstream decompression progress fix](https://github.com/richgel999/miniz/pull/370).
- [SQLite CVE status](https://sqlite.org/cves.html) and
  [release notes](https://sqlite.org/news.html).
- [nlohmann/json advisories](https://github.com/nlohmann/json/security/advisories).
