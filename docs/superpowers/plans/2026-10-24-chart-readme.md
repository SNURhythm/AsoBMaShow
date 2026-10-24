# Chart Readme Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Read artist notes from Reveal in both selectors using beatoraja's sibling `.txt` discovery rule.
**Architecture:** Shared bounded discovery/decoding service plus a reusable asynchronous readme modal; thin scene wiring.
**Tech Stack:** C++23, existing ArchiveFile index/read APIs, SDL views, Android DocumentsContract.
**Spec:** `docs/superpowers/specs/2026-10-24-chart-readme-design.md`

## Global constraints

- Immediate `.txt` siblings, ignoring extension case; 1024 matches maximum.
- 1 MiB maximum selected file; metadata-only archive discovery, no bulk extraction.
- UTF-8, UTF-16 BOM, CP932/CP949 with an explicit encoding override; no scene pointers in background work.
- Use the current checkout, focused tests, no whole-file formatting or deployment.

## Review focus

- Cancellation/reopening must not publish stale text or join blocked I/O.
- Long lines and documents must remain readable without oversized textures.
- Directories, nested files, unrelated archive entries and SAF names must be filtered.
- Empty, malformed and oversized documents need visible, recoverable states.
- Modal input, resize, dropdown portals and scene teardown must preserve ownership.

## Task 1: Shared reader and Reveal integration

Files: `src/library/ChartReadme.{h,cpp}`, `src/scene/ChartReadmeModal.{h,cpp}`,
`src/scene/{MainMenuScene,MusicSelectScene}.{h,cpp}`, Android native/Activity bridges,
localization catalog, CMake/test catalog, and focused readme tests.

- [x] Add failing discovery/encoding/size tests, including a real indexed archive with unrelated payloads.
- [x] Implement `chart_readme::list`, `chart_readme::read`, and decoding/wrapping helpers with stop tokens and bounded reads.
- [x] Implement direct-child SAF listing using the existing registered tree resolver.
- [x] Add a reusable reader modal with background I/O, dropdown selection and virtualized wrapped rows.
- [x] Wire Readme in both selector menus and integrate update/input/resize/teardown.
- [x] Verify focused reader/UI/lifecycle/localization tests and desktop main; compile Android for native bridge coverage.
- [x] Review independently, resolve findings, commit and push to the current upstream.

## Verification

- Desktop `main`, reader and modal test targets built successfully.
- Nine focused CTests passed: reader, modal/text input, bounded archive reads,
  selector event/external-action/scene lifecycle, main-menu lifecycle, Android
  direct-child listing, and localization. Encoding changes passed the three
  affected reader/modal/catalog tests again.
- Android release APK built successfully. Galaxy S20 FE (Android 13) verified
  Reveal → Readme, Korean CP949 automatic and explicit decoding, a recoverable
  wrong-encoding error, scrolling, and Back returning to the selected chart.
- Independent review limit-reporting findings were fixed; the follow-up review
  of strict legacy decoding, JNI, cancellation and dropdowns found no important
  actionable issues.
