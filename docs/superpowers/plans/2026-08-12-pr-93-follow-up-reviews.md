# PR 93 Follow-up Reviews Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Apply the current actionable PR 93 review findings without reintroducing behavior that the user explicitly rejected.

**Architecture:** Preserve all replay lane-cover transitions in their recorded order, make explicit live Hi-Speed values—including zero—distinct from legacy fallback, and reproduce Java `int` arithmetic without C++ signed overflow. Course export must carry the effective random skin selection from timing preflight into the one-stage renderer without retaining every stage's GPU resources.

**Tech Stack:** C++23, CMake/CTest, Lua skin session model, pinned Beatoraja source.

## Global Constraints

- Filter out resolved/outdated threads and Codex usage-outage notices.
- Preserve the user's source-of-truth decisions: visible skins stay editable mid-session and lane-cover controls remain available.
- Compatibility-sensitive Hi-Speed checks were re-read from pinned Beatoraja `c2ed5db1a46145ed10790c3872f717e95b59db9d`.
- Use a red-green test for every production behavior change.
- Do not deploy; push only the reviewed branch after verification.

---

### Task 1: Preserve all replay lane-cover changes within one export frame

**Files:**

- Modify: `src/scene/play/ReplayVideoGameplayPreflight.{h,cpp}`
- Modify: `src/scene/play/ReplayPlayfieldPresentation.{h,cpp}`
- Modify: `src/ReplayVideoExporter.cpp`
- Test: `tests/replay_playfield_presentation_tests.cpp`

- [x] Add a failing test containing a value adjustment followed by an enable toggle at the same frame timestamp.
- [x] Run `replay_playfield_presentation_tests` and confirm the current playback retains only the final change kind.
- [x] Return ordered lane-cover transitions and apply each transition to the presentation's Hi-Speed state before the final frame authority snapshot.
- [x] Rerun `replay_playfield_presentation_tests` green.

### Task 2: Make Hi-Speed zero and green-number arithmetic source-compatible

**Files:**

- Modify: `src/scene/play/BeatorajaHiSpeed.h`
- Modify: `src/scene/play/PlayfieldVisualState.h`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Test: `tests/beatoraja_hispeed_tests.cpp`, `tests/builtin_renderer_characterization_tests.cpp`, `tests/play_skin_state_bridge_tests.cpp`

- [x] Add failing tests for an explicit zero configured Hi-Speed and a near-`INT_MAX` duration.
- [x] Run the focused tests and verify legacy fallback/overflow behavior is observed.
- [x] Represent a supplied speed as optional, keep zero authoritative, and use defined Java-compatible 32-bit multiplication/division semantics.
- [x] Rerun the focused Hi-Speed, renderer, and skin-property tests green.

### Task 3: Pin a Random skin selection between course timing preflight and rendering

**Files:**

- Modify: `src/skin/beatoraja/BeatorajaSkinConfiguration.h`, `LuaSkinTableDecoder.{h,cpp}`, `PlaySkinSession.{h,cpp}`
- Modify: `src/scene/play/GameplaySkinSessionFactory.{h,cpp}`, `ReplayPlayfieldPresentation.{h,cpp}`, `ReplayVideoGameplayPreflight.{h,cpp}`, `src/ReplayVideoExporter.cpp`
- Test: `tests/lua_skin_table_decoder_tests.cpp`, `tests/replay_playfield_presentation_tests.cpp`

- [x] Add a failing configuration-reconciliation test that requests Random but supplies an already chosen runtime option/file.
- [x] Run the focused skin-model/replay tests and confirm the stage boundary initially lacks the carried selection.
- [x] Carry the non-persisted effective option/file mapping out of the course timing session and into the later stage session; keep profile values and configuration digests at their saved Random sentinels.
- [x] Rerun the focused skin-model and replay-presentation tests green.

### Task 4: Review, verify, commit, and push

- [x] Confirm the course combo-break report is already fixed at the current head and make no duplicate edit.
- [x] Run the relevant desktop targets, full desktop CTest, and unsigned iOS release verification.
- [ ] Check the staged diff, commit the review fixes, and push `feature/luaskin`.
