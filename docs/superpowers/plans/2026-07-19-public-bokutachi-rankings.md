# Public Bokutachi Rankings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fetch and paginate Bokutachi chart rankings without an API key while preserving optional authenticated “You” highlighting and authenticated-only score submission.

**Architecture:** Treat chart resolution and PB retrieval as public driver operations, and isolate authentication to an optional identity lookup. Carry the optional user ID through the parser and version-1 continuation token, while allowing the ranking service to invoke drivers with an empty credential.

**Tech Stack:** C++23, nlohmann/json, CMake/CTest, existing `IrRankingService`, `TachiDriver`, and native Tachi response parser.

## Global Constraints

- Never attach an API key to public chart-resolution or PB-page requests.
- Never put an API key in ranking page tokens, cache rows, or diagnostics.
- Keep score submission and polling API-key requirements unchanged.
- Preserve existing authenticated page tokens with positive numeric `userID` values.
- Anonymous rows must never be marked as the current user, including when a user ID exists in `bokutachi-cache`.

---

### Task 1: Optional ranking identity parser

**Files:**
- Modify: `src/ir/tachi/TachiRankingParser.h`
- Modify: `src/ir/tachi/TachiRankingParser.cpp`
- Test: `tests/tachi_ranking_parser_tests.cpp`

**Interfaces:**
- Consumes: native PB response bodies and `std::optional<std::int64_t>` identity.
- Produces: `parseRankingPageResponse(..., std::optional<std::int64_t>)` with `currentUser` true only for a matching present identity.

- [x] **Step 1: Write an anonymous parser test**

Add a test that parses a valid page with `std::nullopt`, asserts success, and asserts that every entry has `currentUser == false`.

- [x] **Step 2: Run the parser test and confirm it fails to compile**

Run: `cmake --build cmake-build-debug --target tachi_ranking_parser_tests -j 6`

Expected: compilation fails because the parser still requires `std::int64_t`.

- [x] **Step 3: Make identity optional**

Change the parser declaration, definition, and PB mapping helper to accept `std::optional<std::int64_t>`. Reject only present non-positive IDs and calculate the marker as:

```cpp
.currentUser = authenticatedUserId && *userId == *authenticatedUserId
```

- [x] **Step 4: Run the parser test**

Run: `cmake --build cmake-build-debug --target tachi_ranking_parser_tests -j 6 && ./cmake-build-debug/tachi_ranking_parser_tests`

Expected: pass.

### Task 2: Public driver requests and anonymous pagination

**Files:**
- Modify: `src/ir/tachi/TachiDriver.cpp`
- Test: `tests/tachi_driver_tests.cpp`

**Interfaces:**
- Consumes: optional parser identity and existing `IrProviderRuntimeConfig`.
- Produces: public resolve/PB requests, optional `/status` lookup, and cursors whose `userID` is positive or null.

- [x] **Step 1: Write driver tests for anonymous and authenticated public requests**

Cover an empty-key first page, an anonymous continuation, no `Authorization` on resolve/PB requests, optional authenticated status lookup and highlighting, rejection of a non-empty malformed credential, and unchanged submission preflight.

- [x] **Step 2: Run the driver test and confirm failure**

Run: `cmake --build cmake-build-debug --target tachi_driver_tests -j 6 && ./cmake-build-debug/tachi_driver_tests`

Expected: anonymous ranking assertions fail because the driver currently returns `AuthenticationRequired`.

- [x] **Step 3: Implement public ranking transport**

Represent cursor and parser identity as `std::optional<std::int64_t>`, accept JSON null in `userID`, omit `Authorization` from resolve/PB requests, and skip cached/user status identity work when `apiKey` is empty. Retain authenticated status lookup for valid keys and reject non-empty invalid keys before HTTP.

- [x] **Step 4: Run driver tests**

Run: `cmake --build cmake-build-debug --target tachi_driver_tests -j 6 && ./cmake-build-debug/tachi_driver_tests`

Expected: pass.

### Task 3: Ranking service without credentials

**Files:**
- Modify: `src/ir/IrRankingService.cpp`
- Test: `tests/ir_ranking_service_tests.cpp`

**Interfaces:**
- Consumes: possibly empty credential lookup result.
- Produces: driver calls whose runtime config may contain an empty `apiKey`.

- [x] **Step 1: Write a service test for an absent credential**

Request a ranking with no stored API key and assert that the fake driver is called with an empty credential and the service publishes success.

- [x] **Step 2: Run the service test and confirm failure**

Run: `cmake --build cmake-build-debug --target ir_ranking_service_tests -j 6 && ./cmake-build-debug/ir_ranking_service_tests`

Expected: failure because the worker currently returns `AuthenticationRequired` without calling the driver.

- [x] **Step 3: Always invoke the ranking driver**

Construct `IrProviderRuntimeConfig` regardless of credential presence and call either first-page or continuation fetch. Preserve diagnostic redaction with the possibly empty credential.

- [x] **Step 4: Run service tests**

Run: `cmake --build cmake-build-debug --target ir_ranking_service_tests -j 6 && ./cmake-build-debug/ir_ranking_service_tests`

Expected: pass.

### Task 4: Regression verification and publication

**Files:**
- Verify: all modified sources, tests, and documentation.

**Interfaces:**
- Consumes: completed parser, driver, and service changes.
- Produces: verified commit on `feature/bokutachi-ir`, pushed to its upstream PR branch.

- [x] **Step 1: Run focused tests**

Run: `ctest --test-dir cmake-build-debug --output-on-failure -R '^(tachi_driver_tests|tachi_ranking_parser_tests|ir_ranking_service_tests)$'`

Expected: 3/3 tests pass.

- [x] **Step 2: Run the desktop compile check**

Run: `cmake --build cmake-build-debug --target main -j 6`

Expected: successful build.

- [x] **Step 3: Inspect the diff and credential surfaces**

Run: `git diff --check && git diff --stat && rg -n 'Authorization' src/ir/tachi/TachiDriver.cpp`

Expected: clean diff; authorization remains only on submission, poll, and identity requests.

- [ ] **Step 4: Commit and push**

Run:

```bash
git add docs/superpowers/specs/2026-07-19-public-bokutachi-rankings-design.md docs/superpowers/plans/2026-07-19-public-bokutachi-rankings.md src/ir/tachi/TachiRankingParser.h src/ir/tachi/TachiRankingParser.cpp src/ir/tachi/TachiDriver.cpp src/ir/IrRankingService.cpp tests/tachi_ranking_parser_tests.cpp tests/tachi_driver_tests.cpp tests/ir_ranking_service_tests.cpp
git commit -m "feat: allow public Bokutachi rankings"
git push origin feature/bokutachi-ir
```

Expected: commit and push succeed.
