# Skin Parity Review Loop Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the four remaining confirmed gaps, then repeat source comparison, reproduction, fixes, and review across the supported skin paths.

**Architecture:** Preserve the shared skin model and session ownership. Use one LuaJ conversion implementation; project final-course chart properties separately from aggregate result presentation; extend JSON binding decoding to retain Lua callbacks in a session-owned runtime.

**Tech Stack:** C++23, LuaJIT, CMake/CTest; pinned beatoraja Java/LuaJ source and bundled jar.

**Spec:** User request to fix all four findings and start a review loop. Baseline `011d1428`; reference beatoraja `c2ed5db1a46145ed10790c3872f717e95b59db9d`.

## Global Constraints

- Work in the current checkout and branch; commit and push verified work.
- Do not edit external skins, alter Lua file persistence, or add LITONE-specific tests.
- Run one CMake/Ninja build at a time; preserve surrounding formatting.
- Keep Standard safety limits and BeatorajaCompatibility behavior distinct.
- Require source evidence and a failing reproduction for new findings. Record reviewed coverage and residual uncertainty rather than claiming exhaustive equivalence.

## Review Focus

- Numeric strings: trailing junk, ASCII whitespace, hexadecimal syntax, long prefixes, and overflow must match pinned LuaJ.
- Numeric text: fractions, exponent notation, integral doubles, NaN, and infinities must match pinned LuaJ text.
- Course metadata: finished and failed courses expose the last played chart while retaining aggregate score denominators and course titles.
- JSON callbacks: configuration and initial state exist during factory construction; later frames use current state; failures release resources.
- Static JSON and catalog inspection: no unnecessary Lua execution; script-bearing sessions retain callbacks through rendering and event dispatch.

### Task 1: Shared LuaJ conversion parity

**Files:** `LuaJValueCoercion.h`, `LuaSkinHostModules.cpp`, `Skin2DRenderer.cpp`, runtime conversion helpers as needed; existing Lua and draw-command tests.

**Interface:** Shared numeric-string parser and scalar-to-string conversion used by runtime property boundaries.

- [x] Add regressions for invalid numeric prefixes and numeric text using bundled LuaJ outputs as the oracle.
- [x] Build and run focused tests; record expected failures.
- [x] Consolidate numeric parsing and implement pinned numeric text formatting.
- [x] Rebuild and run affected tests, including existing timer/event utility cases.

### Task 2: Final-course chart metadata

**Files:** `ResultScene.cpp`, `ResultPresentationUtils.h`, `ReplayVideoExporter.cpp` if its course path shares the defect; generic result tests.

**Interface:** Result skin data distinguishes aggregate course metadata from last-chart properties.

- [x] Add a course ending at 180 BPM with ANOTHER difficulty; assert skin BPM and difficulty plus unchanged course totals/title.
- [x] Run the focused regression and confirm the current zero/unknown failure.
- [x] Preserve last-chart properties in interactive and exported final-course skins.
- [x] Run result/session/export tests.

### Task 3: JSON Lua bindings

**Files:** `JsonGameplaySkinDecoder.*`, `GameplaySkinDocumentLoader.*`, `LuaSkinRuntime.*`, session setup, existing JSON/session tests, and build wiring if needed.

**Interface:** Static JSON bindings remain value-owned; authored scripts compile into the shared model and retain their runtime for session evaluation.

- [x] Trace pinned JSON serializer behavior and choose the smallest runtime integration consistent with existing sessions.
- [x] Add failing session regressions for expressions, timer factories reading initial state, frame updates, and events; retain static JSON and catalog checks.
- [x] Add JSON callback compilation with initialized configuration/state and normal cleanup.
- [x] Run decoder, runtime, session, and cross-format tests.

### Task 4: Review and fix loop

- [x] Pass 1: Review changed boundaries and adjacent property, timer, event, decoder, and rendering paths against pinned source.
- [x] Reproduce and fix every confirmed actionable mismatch found by the pass.
- [x] Pass 2: Review the resulting code and broaden to untouched supported paths; repeat if further confirmed gaps appear.
- [x] Build all desktop/test targets and run CTest in parallel.
- [x] Record coverage, findings, verification, and remaining limitations; commit and push.

## Execution Record

- Subagent dispatch is unavailable because this conversation has reached its agent-thread limit. Execute and review locally; do not claim an independent review.
- The user has authorized implementation and review-loop execution, so proceed without a plan approval pause.

- Review pass 1 identified JSON factory dispatch differences; pass 2 reproduced a partial-course gauge fallback hidden by unplayed-stage padding. See `docs/skin-compat/2026-10-02-skin-parity-review-loop.md`.

- Final build passed. Full CTest passed 408/409; regenerated the stale fixture hash through the pinned-source oracle generator, then the failed oracle test passed. All 409 tests verified. Numeric formatting also matched a second 200,000-case Java sample.
