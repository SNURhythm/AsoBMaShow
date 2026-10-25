# Chart Resource Case Fallback Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Resolve chart resources with mismatched ASCII casing without repeated directory scans.
**Architecture:** Shared native lazy directory cache behind resource resolution; SAF child-name/document-ID cache behind the existing read bridge.
**Tech Stack:** C++23, Java 17, CMake/CTest and host Java fixtures.
**Spec:** `docs/superpowers/specs/2026-10-25-chart-resource-case-design.md`

## Global Constraints

Exact spelling wins; ambiguous fallback fails; extension order stays unchanged.
Bound retained indexes. No payload scans, parser edits, deployment or worktrees.

## Review Focus

Symlink/parent traversal preserves filesystem meaning; mutations invalidate native
negative entries; oversized directories still resolve; SAF clears folded aliases;
artwork and audio consumers actually use the resolved spelling.

### Task 1: Native resource lookup

- [x] Extend archive resource and decoded-asset tests; observe failure on a case-sensitive volume.
- [x] Add `src/CaseInsensitivePathLookup.h` with bounded cached component lookup.
- [x] Integrate `ArchiveFile.cpp`, `audio/ChartAudioFileLookup.h`, and artwork reads.
- [x] Run native resource/decoded-asset tests and desktop compile.

### Task 2: Android SAF

- [x] Add host fixture exercising Activity resource resolution with case-sensitive provider names; observe failure.
- [x] Add bounded directory-name cache and wire Activity lookup/prewarm/clear.
- [x] Verify nested lookup, ambiguity, query reuse, cache clear, and concurrent requests.

### Task 3: Verification and delivery

- [x] Run affected CTest groups and Java checks; inspect diff and obtain independent review.
- [x] Commit task-related changes and push current upstream.

## Verification and review record

- Native resource lookup, decoded audio, and image availability failed on a
  case-sensitive volume before implementation and pass afterward.
- SAF fixture: one query per directory, 1,000 missing references without added
  queries, exact/ambiguous names, Unicode preservation, overflow, eviction,
  concurrent lookup, prewarming, and clearing.
- Independent review identified a SAF clear/listing race, downstream audio
  lexical normalization of symlink parents, and delayed native cancellation.
  Each was reproduced and fixed with a failing-then-passing regression.
- Source-only review limits: Windows behavior and physical Android providers
  were not exercised. Native case lookup also passed on case-sensitive HFS+
  with coarse timestamps; an ad-hoc 12,000-long-name fixture exercised overflow.
- Local case-sensitive APFS benchmark: 1,000 references against 2,000 files took
  22.8 ms cold and 21.3 ms warm; these are host measurements, not Android timings.
- Desktop `main` and Android restricted-file debug Java compilation passed.
- Focused case-sensitive CTest run: 4/4 passed. Audio/archive/Android group run:
  80/81 passed. The existing `android_folder_picker_lifecycle_tests` fixture
  lacks native document/translation fields; it also fails against unchanged HEAD.
