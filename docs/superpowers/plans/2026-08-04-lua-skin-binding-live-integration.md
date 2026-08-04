# Lua Skin Typed Binding Live Integration Plan

**Goal:** Route every currently modeled gameplay binding through `LuaSkinBindingDecoder`, retain the typed registry in the decoded model, and validate built-in type/domain plus live callback generation before accepting object dependencies.

**Architecture:** Keep Lua table traversal inside the existing protected C callback, but copy only structural data there. After the protected call returns, decode typed bindings through `LuaValueHandle`, materialize objects/destinations/notes, and transfer the decoder registry. Require explicit runtime/built-in context at decode and built-in/callback-liveness context at validation, so no context is silently discarded.

**Security invariants:** Preserve per-kind and aggregate binding budgets, exact typed domains, first-authored ordinals, numeric/name/function/script precedence, and the existing protected Lua boundary. Binding failures carry exact indexed field paths. Invalid optional dependencies disable their objects; invalid critical dependencies fail closed.

---

## Task 1: Add failing decoder and live integration tests

**Files:**
- Modify: `tests/lua_skin_binding_decoder_tests.cpp`
- Create: `tests/lua_skin_binding_live_integration_tests.cpp`
- Modify: `CMakeLists.txt`

1. Add a decoder test for numeric fallback semantics used by authored `ref`/`type`, including StringWriter fallback behavior.
2. Add live table-decoder tests covering all eight kinds, numeric-string precedence, functions/scripts, Timer trial behavior, Event one-argument invocation, binding dedupe/first ordinal, and exact indexed failure paths.
3. Add live validator tests for invalid/dead callbacks and wrong built-in kind/domain, proving optional disable and critical failure.
4. Add a live aggregate-source-budget test.
5. Build/run the new tests and record the expected failures before production edits.

## Task 2: Integrate typed binding decode after protected table copying

**Files:**
- Modify: `src/skin/beatoraja/LuaSkinBindingDecoder.h`
- Modify: `src/skin/beatoraja/LuaSkinBindingDecoder.cpp`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`

1. Extend binding requests with an optional numeric fallback while preserving direct StringWriter numeric rejection.
2. Add an explicit gameplay decode context containing the runtime and immutable built-in catalog.
3. Copy binding-bearing raw fields without coercing them to integers, while retaining structural numeric fallback fields and one-based authored indexes.
4. Move sprite binding, note construction, object materialization, and destination materialization outside `withValueProtected`.
5. Decode image/image-set/value/float/slider/text/graph/destination bindings with exact paths and typed domains; preserve `op`, `draw`, and offset normalization.
6. Transfer all eight typed binding registries into `BeatorajaSkinModel`.
7. Run the decoder/live tests until green.

## Task 3: Enforce validation context and fail-closed dependency behavior

**Files:**
- Modify: `src/skin/beatoraja/SkinModelValidator.h`
- Modify: `src/skin/beatoraja/SkinModelValidator.cpp`
- Modify: existing validator/table-decoder tests and callers under `tests/`

1. Require `SkinBindingValidationContext` in `SkinModelValidator::validate`.
2. Admit only registry entries whose built-ins match kind/domain and whose callback IDs are live in the supplied generation.
3. Build dependency sets from admitted bindings so invalid optional objects are disabled and invalid critical objects fail.
4. Update existing call sites with explicit catalog/liveness fixtures and run all affected tests.

## Task 4: Verify the bounded slice

**Files:**
- Modify as needed only for build wiring/test compatibility.

1. Run binding decoder, live integration, table decoder, model validator, numeric glyph, gauge, text/graph, and runtime tests.
2. Run the Lua gameplay feature-gate checks.
3. Configure a feature-off build and prove the new target is absent.
4. Build `main` with `cmake --build cmake-build-debug --target main -j 6`.
5. Inspect the final diff, ensure the worktree is clean after one focused commit, and report the SHA plus the pinned Gauge-timer limitation.
