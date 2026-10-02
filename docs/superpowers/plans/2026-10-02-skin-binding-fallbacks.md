# Skin binding fallback parity implementation plan

> **For agentic workers:** Use superpowers:executing-plans inline with test-driven development. Earlier dispatches reached the conversation's agent-thread limit.

**Goal:** Continue the requested parity review and fix confirmed property-resolution mismatches.

**Architecture:** Keep serializers and object-loader fallback separate. Preserve the shared model and runtime ownership; change the existing Lua/JSON decoder boundaries only.

**Tech Stack:** C++20, LuaJIT, nlohmann JSON, CMake/CTest, pinned local Beatoraja source.

**Spec:** User's “keep going” continues the review/fix loop. Pinned Beatoraja `c2ed5db1a46145ed10790c3872f717e95b59db9d`: LuaSkinLoader serializer map, JsonSkinSerializer LuaScriptSerializer, JsonSkinObjectLoader numeric/text/slider creation.

## Global constraints

- Work on the current branch; no worktrees or deployment.
- Preserve external skins and Lua file persistence; no LITONE-specific tests.
- No whole-file formatting. One build per build directory at a time.
- Commit and push verified task changes to the current upstream.

## Review focus

- Invalid types and unknown numeric properties resolve to null, allowing ref fallback.
- Failed optional scripts allow loader fallback; resource/cancellation failures remain fatal.
- Explicit float values use rate factories; numeric ref overloads use float factories.
- A missing resolved text writer falls back to ref and enables implicit editability.
- Valid explicit callbacks retain precedence and are not compiled twice.

### Task 1: Property resolution

**Files:** LuaSkinTableDecoder.cpp, JsonGameplaySkinDecoder.cpp, existing decoder tests.

- [x] Add Lua/JSON regressions for invalid/unknown values and distinct float factories.
- [x] Run focused tests and observe failures.
- [x] Apply fallback after serialization; preserve fatal failures.
- [x] Rebuild and run focused suites.

### Task 2: Text writer resolution

**Files:** The same decoders and tests; binding decoder only if required.

- [x] Reproduce invalid explicit writer fields suppressing ref fallback.
- [x] Match pinned JSON numeric-writer rejection and resolved-writer editability.
- [x] Verify valid explicit writers and missing writers across Lua/JSON.

### Task 3: Review and verification

- [x] Review related slider/graph branches for the same failure mode; fix confirmed instances with regressions.
- [x] Review image-set value/ref factories and fix the distinct-domain mismatch with regressions.
- [x] Run full desktop/test build and parallel CTest.
- [x] Record findings, limits, and test evidence.

Report: `docs/skin-compat/2026-10-02-skin-binding-fallback-review.md`.
Delivery: commit verified changes and push to the current upstream.
