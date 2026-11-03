# Output PCM Retention Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans.

**Goal:** Drop native PCM after preparation without changing initial playback.
**Architecture:** Rebase retained source handles onto output PCM after preparation
and committed rate changes. Compact preparation caches onto prepared outputs.
**Tech Stack:** C++23, existing CMake audio tests.
**Spec:** docs/superpowers/specs/2026-11-04-output-pcm-retention.md

## Global Constraints

No content deduplication. No disk access or new work in callbacks. BMSON cuts
original PCM before release. Retain samples on failed rate-transition preparation.

## Review Focus

Source/output aliasing and metadata; cache ownership preventing release; repeated
rate changes; BMSON original-data requirements; cancellation and failed restart.

### Task 1: Single retained PCM allocation

- [x] Update existing handoff and transition tests to require original allocation
  release while retaining exact prepared output; run RED.
- [x] Add SoundData rebasing at per-sound publication and rate commit. Prepare
  BMSON slices through a per-source loader callback before releasing originals.
- [x] Compact cache entries onto prepared outputs. Track original-versus-converted
  provenance and permit explicit original PCM requests for BMSON.
- [x] Extend cache tests for conversion release and original reload. Run focused
  tests, then build tests_audio and run area_audio at -j 6.
- [x] Review changes, commit and push optimize/loaded-audio-memory.

## Verification and review

- Initial transition test failed because native PCM remained owned after commit;
  now verifies exact prepared samples and release of the original allocation.
- Cross-rate cached loading regression failed on an extra tail frame; shared
  duration-aware resampling now passes without retaining original samples.
- Cancellation regression cancels at the full-stem conversion allocation after
  a BMSON slice is published. It failed because the next chart could not prune
  that owner; generated paths are now tracked before publication and it passes.
- Review also identified cache eviction of unconverted hidden sounds and an
  individual-archive-read fallback for BMSON. Unconverted single copies remain
  warm; converted BMSON sources now return to the existing archive batch path.
- Preview tests check original allocation expiry, output reuse, byte-for-byte
  initial resampling, duration, and deliberate re-decoding of discarded BMSON
  originals. Deferred worker references are reset after each source.
- `cmake --build cmake-build-debug --target tests_audio -j 6` succeeded.
- `ctest --test-dir cmake-build-debug -L '^area_audio$' --output-on-failure --no-tests=error -j 6`: 35/35 passed.
- `cmake --build cmake-build-debug --target main -j 6` succeeded (existing third-party warnings only).
