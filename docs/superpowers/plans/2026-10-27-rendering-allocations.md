# Rendering Allocations Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for inline implementation and one final fresh-context review. User authorized optimizing both layouts; work in this checkout.

**Goal:** Remove recurring CPU allocation work from shared UI, skin submission and built-in gameplay rendering.
**Architecture:** Owned resource keys with borrowed lookup; renderer-owned flat preflight scratch and generation-checked tokens; direct sorted ordinal membership.
**Tech Stack:** C++23, bgfx, existing Metal/readback and renderer characterization tests.
**Spec:** docs/superpowers/specs/2026-10-27-rendering-allocations-design.md

## Global Constraints

- Preserve pixels, ordering, fallback, media ownership and frame pacing.
- Retain only renderer-local scratch; resolve resources again each frame.
- No worktrees, deployment, new telemetry or whole-file formatting.
- Use existing test targets and shared allocation observer; focused groups follow repository guidance.

## Review Focus

- Borrowed cache keys must not outlive callers; shader pair keys must remain unambiguous.
- Release/reload and cache misses must retain correct bgfx ownership and exception cleanup.
- Skin storage reuse must reset scissor, suppression and glyph offsets across changing command types and failed preparation.
- Stale/wrong-owner/consumed skin tokens must not submit reused storage; no allocations after preflight commit.
- Timeline lookup must preserve unsorted/duplicate/sentinel behavior without per-frame hash nodes.

### Task 1: Shared resource cache hits

**Files:** src/rendering/ShaderManager.h, src/rendering/UniformCache.{h,cpp}, tests/ui_batch_metal_tests.cpp, CMakeLists.txt.
**Interfaces:** getProgram/preloadProgram and uniform getters accept std::string_view; cache keys own strings on insertion.
- [x] Add observed warm lookups, borrowed-name, pair-key and release/reload coverage to the existing real Metal test.
- [x] Build ui_batch_metal_tests and run its CTest entry; expect allocation assertion to fail before implementation.
- [x] Implement heterogeneous pair/string lookup and run the test; expect zero warm lookup allocations and passing pixel tests.
- [x] Commit task with measured before/after evidence.

### Task 2: Skin submission preflight

**Files:** src/rendering/SkinQuadBatchRenderer.{h,cpp}, tests/skin_quad_batch_renderer_tests.cpp, CMakeLists.txt.
**Interfaces:** prepare/submitPrepared/discardPrepared signatures unchanged; plans become generation-checked tokens, storage belongs to renderer.
- [x] Extend existing fixture with warm multi-segment glyph preflight allocation checks and changing resource/command/failure/stale-token behavior.
- [x] Build/run skin_quad_batch_renderer_tests; expect warm allocation failure.
- [x] Flatten resolved commands/glyph handles and retain scratch vectors across begin; keep whole-frame preflight and token checks.
- [x] Run existing and new tests; expect no warm preflight allocations and unchanged submission behavior. Commit.

### Task 3: Built-in timeline lookup

**Files:** src/scene/play/BMSRenderer.{h,cpp}, tests/builtin_renderer_characterization_tests.cpp, CMakeLists.txt.
**Interfaces:** public rendering interfaces unchanged.
- [x] Extend characterization fixture to measure render-only allocations across small/large traversed timeline lists and unordered input.
- [x] Build/run builtin_renderer_characterization_tests; expect allocation scaling regression before fix.
- [x] Use sorted input directly, sorting into reusable scratch only for unordered input; preserve sentinel rejection.
- [x] Run characterization coverage; expect equivalent output and no allocation growth from warmed membership lookup. Commit.

### Task 4: Integration and delivery

**Files:** docs/reviews/2026-10-27-rendering-allocations.md, PR161 description; tests/main_menu_hidden_selector_fixture.cpp (repair of stale fixture exposed by verification).
- [x] Build affected tests_rendering tests_ui tests_gameplay and main together at -j6; run label union in parallel. Expect all pass.
- [x] Run scripts/ios_firebase_deploy.sh --build-only --skip-init; expect unsigned build success.
- [x] Record allocation measurements and limitations, obtain one fresh review, fix significant findings if any, push current branch and update linked PR161.
