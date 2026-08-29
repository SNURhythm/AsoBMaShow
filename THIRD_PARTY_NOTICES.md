# Third-Party Notices

AsoBMaShow is licensed under GPL-3.0-or-later. Some bundled or linked
third-party components have their own notices and additional distribution
requirements.

## 7-Zip SDK

AsoBMaShow uses the 7-Zip SDK through the vcpkg `7zip` port for indexed archive
listing and extraction of formats such as 7z, RAR, LZH, and ZIPX.

Current vcpkg package checked for this notice: `7zip` 25.1.

License summary:

- 7-Zip copyright (C) 1999-2025 Igor Pavlov.
- Most 7-Zip SDK files are under GNU LGPL-2.1-or-later.
- `CPP/7zip/Compress/Rar*` files are under GNU LGPL with the unRAR license
  restriction.
- Some decoder files are under BSD-2-Clause or BSD-3-Clause licenses.
- Some files are public domain where stated in the source file.

The complete vcpkg-provided 7-Zip notice is checked in at
`assets/legal/7zip.txt`. That file must be included in binary distributions or
their accompanying materials.

RAR caveat:

7-Zip's RAR decompression engine was developed using unRAR source code. Do not
use the 7-Zip RAR-related code in this project to develop a RAR/WinRAR-compatible
archive creator or compressor. AsoBMaShow's RAR support is archive reading and
decompression only.

Release checklist:

- Keep `assets/legal/7zip.txt` in app bundles and release archives.
- When updating the vcpkg `7zip` package, refresh `assets/legal/7zip.txt` from
  `installed/<triplet>/share/7zip/copyright`.
- Preserve the source comment near the RAR format handler in `src/ArchiveFile.cpp`.
- For binary releases, provide the corresponding AsoBMaShow source and build
  scripts, including the vcpkg manifest and any iOS static-library generation
  scripts used to build the shipped binary.

## unarr

AsoBMaShow uses `unarr` on desktop builds for direct offset-based reads of
non-solid RAR4 archives. RAR5 remains handled by the 7-Zip SDK backend.

Current vcpkg package checked for this notice: `unarr` 1.1.1.

License summary:

- unarr is licensed under GNU LGPL-3.0.
- AsoBMaShow is GPL-3.0-or-later, which is compatible with linking LGPL-3.0
  code in this project.

The complete vcpkg-provided unarr notice is checked in at
`assets/legal/unarr.txt`.

Release checklist:

- When updating the vcpkg `unarr` package, refresh its notice from
  `installed/<triplet>/share/unarr/copyright`.
- For binary releases that include unarr, include `assets/legal/unarr.txt` and
  keep the corresponding AsoBMaShow source and build scripts available.

## Font Awesome Free

AsoBMaShow bundles the Font Awesome Free solid icon font for music player
controls.

Current package checked for this notice: `@fortawesome/fontawesome-free` 6.5.2.

License summary:

- Font Awesome Free icons are licensed under CC BY 4.0.
- Font Awesome Free fonts are licensed under SIL OFL 1.1.
- Font Awesome Free code is licensed under MIT.

The complete package license text is checked in at
`assets/legal/fontawesome-free.txt`.

Release checklist:

- Keep `assets/fonts/fa-solid-900.ttf` and `assets/legal/fontawesome-free.txt`
  in app bundles and release archives when music player icon controls are
  shipped.

## FFmpeg

AsoBMaShow links FFmpeg for replay video and audio export. The vcpkg manifest
enables FFmpeg's `gpl` and `x264` features, so the resulting FFmpeg libraries
are distributed under GPL-2.0-or-later rather than LGPL terms.

Current desktop vcpkg package checked for this notice: `ffmpeg` 8.1.2#2.

The complete vcpkg-provided FFmpeg license text is checked in at
`assets/legal/ffmpeg.txt`. The iOS library generation script refreshes this
file from the package used to create the shipped XCFramework.

Release checklist:

- Keep `assets/legal/ffmpeg.txt` in app bundles and release archives.
- When updating FFmpeg or regenerating the iOS XCFramework, refresh the notice
  from the exact package used for the binary.
- Make the corresponding AsoBMaShow source, build scripts, vcpkg manifest, and
  any other GPL-required source available to binary recipients.

## x264

AsoBMaShow links x264 through FFmpeg for H.264 replay video export.

Current desktop vcpkg package checked for this notice: `x264` 0.164.3108#2.
x264 is distributed under GPL-2.0 terms in this build. Its complete
vcpkg-provided license text is checked in at `assets/legal/x264.txt`.

Release checklist:

- Keep `assets/legal/x264.txt` in app bundles and release archives.
- When updating x264 or regenerating iOS libraries, refresh the notice from
  the exact package used for the binary.
- Preserve the GPL source-availability obligations described above for
  binaries that include x264.
