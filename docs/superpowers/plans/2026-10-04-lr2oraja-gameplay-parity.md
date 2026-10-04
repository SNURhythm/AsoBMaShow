# lr2oraja gameplay parity implementation plan

> **For agentic workers:** Use test-driven implementation and independent comparison passes. The user explicitly requested fixing all findings and repeating comparison until no mismatches are found.

**Goal:** Align LR2 gauges and judgement behavior with local `lr2oraja-endlessdream` commit `5233be081abee2a7f824b78aed0d783847deeb2f`, and retain executable evidence of the comparison.

**Architecture:** Keep the compiled rules and authoritative simulation as the gameplay boundary. Preserve the separate upstream Beatoraja ruleset; fix its explicitly named LR2 gauge profiles as well. Record changed semantics in replay descriptors. Compare native outputs with the actual reference Java classes or narrowly extracted reference control flow, not copied expected formulas alone.

**Tech stack:** C++23, Java 17 reference probes, Python comparison harness, CMake/CTest.

**Specification:** User's audit-and-fix request in this session; reference `core/src/bms/player/beatoraja/play/{BMSPlayerRule,GaugeProperty,GrooveGauge,JudgeProperty,JudgeAlgorithm,JudgeManager}.java` at the commit above.

## Constraints

- Work in the current checkout; do not create a worktree.
- Only one CMake/Ninja build per build directory at a time; root coordinates builds.
- Do not format whole files or edit amalgamated parser sources directly.
- No deployment. Commit and push task changes to the current upstream after verification.
- Keep upstream Beatoraja behavior where it is a separately selected ruleset, except explicitly named LR2 gauge profiles covered by this audit.

## Review focus

- Fractional, absent and very small positive TOTAL; recovery and qualification at borders.
- Explicit course gauge constraints; survival death, low-gauge damage reduction and direct Grade use.
- Rejected/played candidates mixed with normal and long notes under all four priority algorithms.
- HCN updates spanning head/tail, misses, multiple lanes, zero elapsed time, and input transitions.
- Recorded ruleset identities and input priority surviving replay; changed outcomes remain playable and indicate a stale saved result.

## Task 1: Gauge parity

Files: `GameplayGaugeRules.cpp`, relevant `GameplayScoreState.h` gauge helpers, `tests/gameplay_gauge_rules_tests.cpp`, policy tests when their old TOTAL expectations change.

- [x] Add and observe failing cases for fractional TOTAL, absent TOTAL with 101 notes, explicit course profiles, LR2 named profiles and Grade damage reduction.
- [x] Correct gauge compilation and calculation using the reference; preserve unrelated Beatoraja profiles.
- [x] Compare actual Java/native gauge values across note-count/TOTAL thresholds, gauge profiles, judgements, rates and gauge values.
- [x] Run gauge and policy tests and retain comparison evidence.

## Task 2: Candidate parity

Files: `GameplayCandidateRules.{h,cpp}`, `GameplayJudgeRules.cpp`, both input-selection paths, relevant simulation/judge tests and provenance wiring.

- [x] Add failing case: normal at 800ms, LN head at 850ms, press at 1000ms produces no selected note with default Combo.
- [x] Add failing cases for Lowest/Duration/Score and propagation through compiled policy/replay.
- [x] Follow the reference candidate replacement, rejection and multi-BAD ordering exactly.
- [x] Compare mixed normal/LN/played candidate sequences and all configured algorithms to reference control flow.

## Task 3: HCN/update parity

Files: `GameplaySimulation.{h,cpp}`, simulation and worker tests.

- [x] Add failing held-HCN case: a single 700ms update applies one half-rate tick, not three.
- [x] Exercise head/tail crossings, zero-time updates, lane order and input-state transitions against the reference update phases.
- [x] Align the LR2 update cadence without silently changing the separate Beatoraja mode.
- [x] Validate live input/update phases and reproducible replay judging; report stale saved results instead of adding update-timing capture.

## Task 4: Repeated comparison, integration and delivery

Files: ruleset descriptor and tests, committed reference probes/fixtures, comparison report.

- [x] Version changed ruleset semantics; rejudge known older input formats with the current rules and expose a stale result without changing stored facts.
- [x] Add reproducible differential coverage with pinned reference provenance.
- [x] Repeat independent source and executable comparisons after each newly discovered mismatch; fix and re-run until the final covered comparison pass reports none.
- [x] Build desktop `main`, run focused tests and full parallel CTest; investigate failures.
- [x] Review the full branch diff against `develop` and record exact coverage and limits.

Delivery: commit and push to the current upstream, then merge PR #120 into
`develop` after verification, as requested.

## Execution record

- Baseline: AsoBMaShow `02d8de38b1e0b42c46d652e902ac8d132e43596e`; clean working tree; branch `fix/legacy-difficulty-table` tracks `origin/fix/legacy-difficulty-table`.
- Baseline checks: 10 focused suites passed; actual Java/C++ timing windows matched for 72,000 rows. Isolated probes demonstrated candidate, TOTAL, course and HCN discrepancies before implementation.
- Execution choice: proceed within the user's explicit fix-and-repeat authorization. Independent gauge and candidate tasks may run concurrently; simulation edits and builds are coordinated centrally.
- User clarification: "just let the replay result indicate stale result, if the play is reproducible that's ok". Preserve playback under current rules and signal disagreement instead of trying to reproduce historical update scheduling.
- Comparison cycle 2: found Java/C++ default-TOTAL floating-point contraction, per-update HCN carry behavior, input-before-miss ordering, exact-tail LN boundary, and CN/HCN scratch reversal consuming an additional normal note. These enter the fix-and-comparison loop.

- Comparison cycles 3–5: corrected equal-time physical-key snapshots, directional
  scratch processing and classic re-press recovery, initial passing-note bounds,
  and deferred release FAST/SLOW timing. The final runtime matrix includes 1,348
  cases against the full unchanged JudgeManager and currently reports zero
  mismatches. Separate final gauge, candidate, and window runs report
  220,820 / 57,664 / 72,000 comparisons with zero mismatches.
- Replay integration keeps historical input playable with stale results, applies
  current course constraints when rejudging older rules, and preserves the
  separate Beatoraja arrival order.
- User IR clarification: preserve earlier eligibility unless discrepancies would
  materially harm community fairness; the user emphasized CN/HCN is uncommon in
  IR submission. Preserve previously verified LR2 v4 eligibility and frozen
  payloads under their original proof, validating the old TOTAL/candidate policy.
  Do not revive previously unsupported v3 or rewrite any historical proof.
- Final review corrected the Records UI's obsolete-revision IR veto and the
  legacy practice HCN initial passing bound. Extracted production-method tests
  cover zero, negative, and positive initial times; the new regression failed
  before the fix and passed afterward.
- Final verification: desktop and all test targets build; all 420 CTest tests
  pass after the final review correction. The pinned comparison matrices report
  zero mismatches across 351,832 covered cases.
