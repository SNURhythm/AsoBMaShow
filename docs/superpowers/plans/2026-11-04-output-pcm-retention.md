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

- [ ] Update existing handoff and transition tests to require original allocation
  release while retaining exact prepared output; run RED.
- [ ] Add SoundData rebasing at per-sound publication and rate commit. Prepare
  BMSON slices through a per-source loader callback before releasing originals.
- [ ] Compact cache entries onto prepared outputs. Track original-versus-converted
  provenance and permit explicit original PCM requests for BMSON.
- [ ] Extend cache tests for conversion release and original reload. Run focused
  tests, then build tests_audio and run area_audio at -j 6.
- [ ] Review changes, commit and push optimize/loaded-audio-memory.
