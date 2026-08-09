# Beatoraja Lua gameplay skin desktop review

Reviewed commit: `e4f9ee10`

Pinned Beatoraja source: `c2ed5db1a46145ed10790c3872f717e95b59db9d`

Review date: 2026-08-09

## Verified desktop evidence

- `scripts/check_beatoraja_reference.py --require-clean` verified the pinned
  reference at the recorded commit with a clean tree.
- `cmake --build cmake-build-debug --target main -j 12` completed successfully.
- Full desktop CTest completed with **256/256 tests passed** in 116.48 seconds.
- The focused reference, acceptance, iOS-build-setup, and iOS-artifact Python
  suite completed with **138 tests passed** and one expected skip: the optional
  live pinned-reference coupling probe requires an explicit root.
- The external ModernChic 4.6 audit verified the committed reference manifest
  against the reviewed archive and corresponding extracted root. This audit did
  not copy package payload into the repository.

## Reviewed remediation decisions

- `b5aadf50` closes the POSIX archive-input race by opening the selected archive
  with `O_NOFOLLOW`; the focused importer regression proves a linked archive
  is rejected before publication.
- `23f12e74` restores authored skin lane precedence for overlapping touch
  geometry. Virtual-controller controls still use nearest-hit selection only
  within their own interaction layer.
- `e4f9ee10` replaces the invented render-time filesystem freeze with acceptance
  schema v2. It follows pinned `SkinLuaAccessor.RestrictedIoLib` and
  `LuaSkinLoader`: I/O stays ordinary and live inside the selected root. The
  acceptance record stores only observed operation kinds and an opaque evidence
  ID; it does not claim an overlay digest, denied-I/O counter, or synthetic
  fallback.

## Physical acceptance boundary

Gameplay functionality has been manually confirmed by the user. That is valid
product feedback, but it is not substituted for the still-pending physical
performance, screenshot, device, and external-evidence records. No deployment
or upload was performed during this review.
