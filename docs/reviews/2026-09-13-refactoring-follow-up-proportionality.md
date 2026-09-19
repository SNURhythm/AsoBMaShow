# Refactoring follow-up proportionality review

Reviewed 2026-09-13 at source commit 383ec028, covering all 66 entries in
[the follow-up record](../refactoring-next-targets.md). The user requested that
the broad sweep end, then asked whether these changes cover real cases and
whether protections for rare cases impose disproportionate cost.

## Decision

Retain the 66 changes. The review found no demonstrated disproportionate
production runtime cost and no production change that should be reverted on
the available evidence. This is not a claim that all 66 fixed observed user
incidents. Many address transient resource failures reproduced only by injection.

The classification is 7 ordinary-workflow entries, 12 mixed entries, 12
duplication/dead-code cleanups, 26 exceptional/fallback entries, 7 test-only
entries, and 2 defensive reentrancy entries. The rare-case changes mostly
reorder existing work, retain ownership until transfer succeeds, or add local
rollback. They do not introduce a general-purpose task/transaction framework.

## What the evidence establishes

- Ordinary means reachable without injected allocation/thread failure; it
  does not establish field frequency. Mixed entries combine ordinary workflows
  and exceptional-path coverage.
- Exceptional means resource-failure or fallback-only coverage. Most use
  controlled failure injection against production implementations. Entry 13
  relies on joining-thread ownership semantics and existing workload tests,
  without its own direct failure reproducer. Entry 48 concerns compatibility
  code that the current Android configuration does not select.
- Defensive entries 54 and 55 use purpose-built callback destructors to
  demonstrate reentrancy under locks. No occurring product deadlock was shown.
- One-shot allocation tests establish transient exception safety and retry
  behavior. They do not establish recovery under sustained memory exhaustion.
- Costs in the table are source-based assessments unless a measurement is
  explicitly given below. Native dialogs and mobile runtime performance were
  not newly verified in this audit.

## Measured cost and size

The comparison base is 6e8cb6a6, immediately before follow-up 1. Git numstat
through 383ec028 gives:

| Area | Added lines | Removed lines | Net |
|---|---:|---:|---:|
| Production src/ | 2,110 | 2,129 | −19 |
| Tests | 6,942 | 162 | +6,780 |
| CMake/build definitions | 518 | 93 | +425 |
| Documentation | 2,360 | 72 | +2,288 |

Line counts are a maintenance indicator, not a complexity proof. The largest
growth is test scaffolding and evidence, rather than production code.

The final app/all-target build passed, followed by all **399 tests in 102.29 s**.
The pre-follow-up record reports 371 tests in 124.91 s, but those runs were not
a controlled comparison; the differing times do not prove a speedup.

The current music_select_error_flow_contract entry took **91.42 s** and
compiles each fixture in a fresh temporary directory. Most of this behavior
predates the follow-ups. Only two test methods were added to that Python suite
by the 66 entries, although existing fixtures were also extended. Timing those
two added cases separately on the current machine gave:

| Added case | Compiler subprocess | Executable subprocess | Whole test |
|---|---:|---:|---:|
| Direct destruction, entry 7 | 1.786 s | 0.264 s | 2.051 s |
| Course admission failure, entry 53 | 0.792 s | 0.217 s | 1.012 s |

These are isolated current-case timings, not the incremental wall time of the
parallel suite. They show that compilation dominates their recurring cost.
The seven new runners in entries 45–66 together took approximately 0.10 s in
the final suite. Their current binaries/object files total about 30 MiB in
apparent file size, excluding shared dependencies and debug bundles. Neither
number includes all test additions across entries 1–66. The seven targets are
stable_hash_tests, thread_compat_tests, thread_compat_fallback_tests,
settings_preview_chart_tests, play_option_summary_tests,
profile_database_ownership_tests, and text_view_font_lifetime_tests. The size
measurement sums each executable and its target directory's object files; it
does not measure allocated disk blocks or isolate shared build costs.

Two source-identified successful-path costs were checked with an optimized
standalone microbenchmark on this macOS machine:

| Operation | Median of 7 samples | Observed range |
|---|---:|---:|
| Uncontended JukeboxSchedulerWake::capture() | 8.772 ns | 8.655–9.398 ns |
| std::thread create and join | 15.763 μs | 15.256–26.480 μs |
| std::jthread create and join | 15.848 μs | 15.401–18.529 μs |

Capture samples contain two million calls; thread samples contain one thousand
empty-worker creations/joins after warmup, with alternating measurement order.
The thread difference is within the observed variation. These measurements do
not establish mobile performance, contended-lock latency, or end-to-end audio
performance. They do not show a material uncontended/native cost.

## Costs worth retaining in view

1. **Fixture maintenance is the main cost.** Extracted scene tests execute
   production method bodies but mirror surrounding scene shells and depend on
   exact signatures. Future edits can require changes to both code and fixture
   extraction. The recurring compilation at
   [the Python fixture helper](../../tests/music_select_error_flow_contract_tests.py)
   is a possible later build optimization, not a reason to remove coverage.
2. **Entry 37 has the most complicated rare-case admission change.**
   [ChartLibraryTaskService](../../src/library/ChartLibraryTaskService.cpp) adds lifecycle/
   state coordination and staged publication: +44 net production lines and
   233 test lines in its implementation commit. It repairs demonstrated
   ownership failures within the existing service. A generic transaction
   framework would not improve this tradeoff.
3. **Not every guard is free on successful paths.** Entry 44 adds a mutex
   capture in the visual scheduler, measured above. Entries 11/13 can add
   joining-thread stop-state bookkeeping. Entry 46 adds a startup gate and
   notification. Entry 55 releases queue capacity when clearing a scene, so
   reuse may allocate storage again. These are bounded costs; only the capture
   and native thread microbenchmarks were measured here.
4. **Observed ordinary leaks deserve distinction from hypothetical failures.**
   Entry 62's real WebP decoder fixture went from five leaks totaling 25,600
   bytes to zero. Entries 19/24/25 removed actual fixture-directory leaks.
   Their cleanup work is the intended behavior, rather than avoidable overhead.

The audit found one documentation correction: the allocation-lifetime probe
now has three synchronous test consumers, not two. No production changes were
made as a result of this retrospective review. The uncommitted extreme-seek
adapter experiment was already discarded and is not one of the 66 entries.

## Entry-by-entry assessment

| # | Coverage | Case covered | Incremental cost assessment |
|---:|---|---|---|
| 1 | Ordinary | Overlapping cache jobs and superseded status delivery. | Existing workers; small admission/publication locking. |
| 2 | Ordinary | BEST replay load, replacement, cancellation and missing-file fallback. | Reuses a task owner; small callback/token bookkeeping. |
| 3 | Mixed | Concurrent archive access, cancellation and old-flight failure followed by retry. | One owned flight replaces four maps; checkpoint interval unchanged. |
| 4 | Mixed | Library jobs, completion delivery and destruction with work in flight. | Existing worker; typed status channels add modest locking. |
| 5 | Ordinary | Fast Find BMS completion before UI consumption. | Pending-result admission check; existing worker and bounded queue. |
| 6 | Mixed | IR preparation/cancellation around durable enqueue and teardown. | Existing preparation work; small mailbox and gate bookkeeping. |
| 7 | Mixed | Direct scene destruction, including incomplete initialization. | Guarded teardown; relatively large scene fixture and recurring compilation. |
| 8 | Mixed | Profile import/export completion, native source retention and teardown. | Small typed worker owner; no additional operation thread. |
| 9 | Mixed | Chart Viewer destruction while listening, including unwinding. | Guarded audio stop before release; no new owner abstraction. |
| 10 | Mixed | Music Player destruction with video/fullscreen ownership. | Three ownership checks; cleanup only for resources actually acquired. |
| 11 | Mixed | Parallel work and helper consolidation, plus extreme counts/partial launch failure. | Less duplicate code; joining threads add possible stop-state bookkeeping. |
| 12 | Ordinary | Stop arriving between the sleep timer's predicate check and wait. | One extra mutex scope during shutdown; deterministic race test. |
| 13 | Exceptional | Later thread creation fails after earlier workers start. | Four joining-thread substitutions; per-worker ownership bookkeeping. |
| 14 | Exceptional | Allocation failure while constructing persistent worker pools. | Local catch reuses shutdown; no added successful-path work. |
| 15 | Mixed | Intro destruction with subscriptions and partial registration failure. | Guarded existing unsubscriptions; larger integration fixture. |
| 16 | Exceptional | Second input registration throws during construction. | Small rollback catch; normal input delivery unchanged. |
| 17 | Deduplication | Unused application thread registry. | Removes storage and an empty shutdown loop. |
| 18 | Mixed | Picker result/admission race and worker-start failure. | Reorders existing state operations; generated platform fixtures. |
| 19 | Test only | Read-only skin snapshots survive ordinary fixture teardown. | No application cost; required permission restoration/removal. |
| 20 | Exceptional | Replay task startup throws and leaves active ownership. | Failure-only cancellation; existing runner extended. |
| 21 | Exceptional | Preload startup throws and leaves stale pending work. | Failure-only pending-request cleanup; existing runner extended. |
| 22 | Deduplication | Unreachable catalog rebuild and obsolete owner references. | Removes unused code and state. |
| 23 | Exceptional | Export thread startup throws and scenes remain busy. | Local catch reuses result channel; substantial scene test coverage. |
| 24 | Test only | Benchmark leaves immutable fixture directories behind. | Shared cleanup helper; traverses only owned fixture trees. |
| 25 | Test only | Ordinary lifecycle test runs leave snapshot roots behind. | Four callers reuse cleanup; no application cost. |
| 26 | Test only | Duplicated snapshot cleanup loops. | Equivalent cleanup with shared symlink handling; fewer test lines. |
| 27 | Test only | Fixture root collision deletes an existing sentinel. | Exclusive directory creation; retries only on collision. |
| 28 | Exceptional | Gameplay worker startup failure prevents retry. | Catch/reset on failure; no gameplay-loop addition. |
| 29 | Exceptional | Path allocation escapes a noexcept image probe. | Moves an existing operation inside try; no extra normal work. |
| 30 | Test only | Long temporary directory exceeds Unix socket path capacity. | One fixture length check; no application cost. |
| 31 | Exceptional | Folder-request allocation/start failure poisons admission. | Reorders existing preparation/publication; no new owner. |
| 32 | Exceptional | Directory worker startup fails after callback publication. | Existing startup moved earlier; same successful-path operations. |
| 33 | Test only | Catalog fixtures collide with existing directories. | Exclusive root creation; no application cost. |
| 34 | Exceptional | Cache worker callback throws instead of returning a failure result. | Catch boundary; no new worker or traversal. |
| 35 | Ordinary | Symlink-populated cache reports target bytes as removable cache bytes. | No-follow classification; same traversal complexity. |
| 36 | Exceptional | IR preparation/start failure strands service admission. | Failure-only reset under existing mutex. |
| 37 | Exceptional | Allocation failure corrupts library admission/reservations/tokens. | Extra admission locking/staging; largest rare-case reasoning cost. |
| 38 | Deduplication | Repeated Jukebox lifecycle state bindings. | Borrowed aggregate; no new allocation or lock. |
| 39 | Deduplication | Repeated profile/picker scope cleanup implementations. | Existing ScopeExit replaces local forms; stack-only cleanup. |
| 40 | Deduplication | Duplicate gameplay/result timing calculations. | Same calculation; numerical tests replace source/formula checks. |
| 41 | Deduplication | Repeated Music Select mode conversion/filtering. | Equivalent inline predicates; no extra pass or allocation. |
| 42 | Deduplication | Identical clear-lamp conversions. | Same threshold comparisons; distinct policies remain separate. |
| 43 | Deduplication | Identical export/cache filename sanitization. | Same scan/allocation; explicit limits and fallbacks. |
| 44 | Ordinary | Observed paused-stop test failure and reproduced lost scheduler wake. | Added mutex capture per scheduler iteration; measured separately above. |
| 45 | Deduplication | Identical stable hashes and hexadecimal formatting. | Same algorithms; one small header and tiny golden-vector runner. |
| 46 | Exceptional | Scheduler startup allocation fails after audio starts. | Startup gate/notification; no extra steady-state scheduler work. |
| 47 | Exceptional | Selected-chart worker admission fails after analysis publication. | Assignment ordering/pointer move; no new allocation. |
| 48 | Exceptional | Fallback stop-request contract mismatch. | Current Android selects native support; two tiny compatibility runners. |
| 49 | Ordinary | Failed launch completion arrives while application is backgrounded. | One foreground check on failure; shared failure cleanup. |
| 50 | Exceptional | Allocation fails after consuming a preloaded chart. | Existing pointer remains owned through setup; no new allocation. |
| 51 | Exceptional | Profile-export exception leaks an open directory stream. | Stack owner; ordinary descriptor/stream operations unchanged. |
| 52 | Exceptional | Settings preview allocation fails between creation and insertion. | Stack ownership transfers; parser-linked test runner. |
| 53 | Exceptional | Launch capture/thread allocation failure leaves selector busy. | Shared failure cleanup; one added compiled scene case. |
| 54 | Defensive | Purpose-built callback destructor republishes while task cleanup holds a lock. | Moves destruction outside the lock; no product deadlock demonstrated. |
| 55 | Defensive | Purpose-built callback destructor posts during scene queue clearing. | Constant-time swap; future scene reuse may allocate queue storage again. |
| 56 | Exceptional | Database/session construction fails during ownership handoff. | Owning parameters replace raw transfer; same normal allocations. |
| 57 | Exceptional | Synthetic lane-summary construction fails before insertion. | Stack ownership only; parser-linked runner and shared allocation probe. |
| 58 | Exceptional | SQLite error reporting also encounters allocation failure. | Existing resource owner adopted earlier; no extra normal allocation. |
| 59 | Deduplication | Viewer duplicates the replay lane-summary formatter. | Reuses existing implementation; removes fixture extraction dependency. |
| 60 | Deduplication | Fixture-only database opener resides in production support. | Moves existing helper into test support; zero application runtime cost. |
| 61 | Exceptional | Migration SQL error is followed by diagnostic allocation failure. | Existing SQLite error owner; normal SQL/logging unchanged. |
| 62 | Mixed | Ordinary WebP decodes leak buffers; cancellation also exposes an ownership gap. | Required frees reclaim memory; no new allocation or abstraction. |
| 63 | Mixed | Font teardown allocates lookup strings; allocation failure terminates cleanup. | Borrowed tuple lookup removes temporary strings; still O(log n). |
| 64 | Exceptional | Font construction fails after acquiring runtime/cache references. | Two stack guards and shared cleanup; no new successful-path allocation. |
| 65 | Exceptional | View construction/layout callbacks throw, leaking a node or poisoning depth. | Stack guards; no new heap work, but header changes trigger broad rebuilds. |
| 66 | Deduplication | Memory-audio entry points repeat identical virtual-I/O setup. | Delegates to existing bounded decoder; no new policy or owner. |
