# Selector SQL Paging Implementation Plan

> Use subagent-driven-development for the independent repository and provider work.

**Goal:** Eliminate whole-folder C++ indexing before the first visible page.

**Architecture:** Repository queries resolve filters/count, return ordered pages,
and locate identities; a bounded row provider retains existing Scene lifecycle.

**Tech Stack:** C++23, SQLite, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-09-08-selector-sql-paging-design.md`

## Constraints

- Preserve raw SHA representative, reverse-order ties and selector filter rules.
- Keep unrelated review documents and user data untouched.
- Use failing tests before implementation, scoped edits, and one final push.

## 1. Repository query contract

- [x] Define `ChartSelectorQuery` carrying folder, requested/resolved filters,
  sort ID, selected long-note mode and immutable score caches.
- [x] Add failing production-query differential tests for count, pages and IDs.
- [x] Implement `ResolveChartSelectorQuery`, `SelectChartSelectorPage` and
  `FindChartSelectorIndex` on `ChartRepository::Session`, with cancellation.
- [x] Verify query plans and add matching indexes only where justified.

## 2. Provider and physical-folder integration

- [x] Add tests showing opening decodes bounded records, not a whole-folder index.
- [x] Implement a SQL-backed `MusicSelectRowProvider` with the shared 128x6 cache.
- [x] Keep clone/configure/selection/error behavior and replace normal physical
  directory loading; retain explicit autoplay behavior.
- [x] Run existing repository, provider, navigation and lifecycle regressions.

## 3. Performance and verification

- [x] Benchmark real SQLite count and first-view preparation against both old
  selector indexing and MainMenuScene-style paging on identical data.
- [x] Independently review SQL parity, query plans and provider ownership.
- [x] Build main/all tests, run parallel CTest and unsigned iOS build-only.
- [x] Record measured results and limitations.

Final delivery: commit verified changes and push once.

Results and the unrelated parallel renderer-test limitation are documented in
`docs/superpowers/2026-09-08-selector-sql-paging-validation.md`.
