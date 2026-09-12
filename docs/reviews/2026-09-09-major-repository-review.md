# Major repository review — September 9, 2026

Status: **complete — all 22 findings fixed or verified, independently reviewed, with converged validation passed**. No confirmed major finding remains open within this review's coverage; this is not a claim of defect-free code or universal release readiness.

## Scope and baseline

PR #103 was merged normally into `develop` at `de1c8d45d4fdf50429bc2fbe40b9d990859be7b1`. This cycle starts from that commit on `fix/repository-major-review-2026-09-09`. The five supplied September 8 review documents are preserved unchanged; their findings are revalidated against the merged baseline rather than assumed still present.

All 11 supplied findings receive a disposition. New findings require a concrete major impact: reachable process failure, durable incorrect results, a credential or filesystem trust violation, broken primary workflows, or substantial unbounded resource consumption. Style issues, micro-optimizations and speculative hardening are not counted.

The independent frozen-baseline review identified nine additional major findings, initially two P1 and seven P2; runtime evidence later refines RENDER-01 to P2 functional failure rather than a demonstrated crash. Subsequent startup investigation reproduced two additional P1 failures: portable legacy-score upgrade and Android SQLite temporary storage, bringing the total to 22 supplied/new findings. Closure requires bounded regressions, corrections or verified baseline mitigation, and independent review. This count excludes follow-up defects within the same root cause.

## Supplied findings

| Finding | Baseline disposition / correction | Status | Commit |
| --- | --- | --- | --- |
| COR-01 | Manual practice now completes only at its actual boundary. | Fixed, independently reviewed | `59a1468` |
| COR-02 | Start+Select abort records a failed, fully accounted terminal attempt and prevents unfinished course advancement. | Fixed, independently reviewed | `2de2fa23` |
| COR-03 | Course continuation preserves live gauge/combo carry when authored LN mode differs from the selected fallback. | Fixed, independently reviewed | `c64927c9` |
| COR-04 | Modern course capture records effective played and future stage note counts. | Fixed, independently reviewed | `d82accfc` |
| COR-05 | Fresh retry/viewer/practice preparation normalizes effective notes before policy/provenance capture. | Fixed, independently reviewed | `a7580b37` |
| SEC-01 | The merged baseline already rejects non-web/untrusted shell targets before OS dispatch. Added action-path/shared-predicate regressions; no duplicate production patch. | Verified, independently reviewed | `3457e6ec` |
| SEC-02 | Expansion writers and packed/extracted BMS verification now enforce distinct resource budgets and cooperative cancellation; failures cannot become a Keep Files choice. | Fixed, independently reviewed | `83cb77d1`, `ac9b48a9` |
| SEC-03 | Existing exception/cancellation cleanup is verified with actual copy bodies; archive-only byte/storage admission now bounds private inbox copies. | Fixed, independently reviewed | `a856cc2d` |
| SEC-04 | Deferred polling could combine original server A with current server B's credential. The driver now binds polling to the configured credential origin; origin changes require an authoritative confirmed-absent credential read. | Fixed, independently reviewed | `ca671654` |
| PERF-01 | Skin movies use a paused source clock rebased before initial/forward/backward/wrapped presentation. | Fixed, independently reviewed | `941307b0` |
| PLAT-01 | Both Android flavors admit intended legacy public HTTP sources while retaining existing HTTPS-origin downgrade and authenticated IR restrictions. | Fixed, independently reviewed | `1bf313c1` |

## New major findings

| Finding | Severity | Concrete impact | Status | Commit |
| --- | --- | --- | --- | --- |
| GAME-01 | P1 | Queued exit now rechecks realtime authority before accessing touch-router state. | Fixed, independently reviewed | `d8dde95` |
| GAME-02 | P2 | Ordinary/retry/course preparation now applies DP-FLIP before independent player modifiers; partial-course Retry Same restores saved FLIP/P2 at the verified session factory. | Fixed, independently reviewed | `ee8a8b51` |
| GAME-03 | P2 | Duration-driven display arrays now use checked admission and explicit omission across selector/gameplay/result/replay/course consumers, without truncating playable time or durable event history. | Fixed, independently reviewed | `43b51a5c` |
| SEC-NEW-01 | P2 | Desktop Find BMS GET/POST callbacks now admit bounded response bytes before append and propagate worker cancellation. | Fixed, independently reviewed | `6111d905` |
| SEC-NEW-02 | P2 | The default IR waiter absorbs notifications arriving between work inspection and its late wake-revision snapshot, stranding accepted work. | Fixed, independently reviewed | `73ac7fe4` |
| RENDER-01 | P2 | Lua coroutine argument copying grows its own loop bound; real argument-bearing skins/callbacks fail with protected stack overflow on the tested LuaJIT build. | Fixed, independently reviewed | `f9c7cc22` |
| RENDER-02 | P2 | Chart audio duration/tail/work admission now bounds dense rendering; block conversion and owned atomic staging preserve complete output on cancellation or error. | Fixed, independently reviewed | `f3d31ddd` |
| PUI-01 | P2 | Firebase on Android API 28/29 now retains SAF instead of assuming raw storage authorization. | Fixed, independently reviewed | `d6c954ce` |
| PUI-02 | P2 | iOS Find BMS archives remain in bounded, cancellable file staging and publish atomically without complete NSData/vector buffers. | Fixed, independently reviewed | `b2d99e62` |
| DATA-01 | P1 | Existing v0 score databases cannot upgrade because consecutive migration passes attach the same chart alias inside one atomic transaction, blocking profile startup. | Fixed, independently reviewed | `be38026b` |
| DATA-02 | P1 | Android has no writable default SQLite temp location; creating a fresh replay schema fails during transactional table retirement and prevents profile startup. | Fixed, independently reviewed | `4e9111e9` |

## Completed evidence

### IR and external URL boundaries

The real Tachi driver/service/settings regressions use an in-memory credential backend and captured HTTP requests, not real accounts. They cover automatic/manual work, blocked original job identity, incompatible destination/key rejection, explicit remove/change/add recovery through the same GET job without another POST, and settings reactivation. Independent review found and closed a further same-root-cause bypass: a transient credential-read failure had been interpreted as absent display state. Both recovery-before-save and still-failing reads now fail closed before any settings publication or reactivation to the incompatible origin.

The lost-wake regression uses the production condition-variable waiter, with no injected waiter/wake implementation. Controlled barriers place a single notification inside the actual inspection-to-wait interval for both reconciliation and due uploads; completion requires no rescue notification or periodic polling. Mutation/baseline runs fail and the correction passes.

The external-URL regression covers both table download action fields and the actual shared HTTP(S) predicate, including local paths, UNC paths, non-web schemes, embedded controls/NUL and backslashes. The shared opener's rejection before OS dispatch was inspected in source. This is not an instrumented OS-dispatch or end-to-end table-import runtime test.

The final IR correction builds `main` and four focused targets. The implementer's final run passes all four suites three times (12 executions, 15.72 seconds). A fresh controller build and four-suite run pass independently (5.27 seconds). Both independent review gates are closed. These focused results are supplemented by the converged full-suite/mobile gates below.

### Gameplay terminal integrity

Manual-practice boundary and queued realtime-exit regressions exercise extracted production scene methods. Aborts stop/synchronize the worker before finalizing remaining notes and zeroing all gauge/combo facts. Actual capture, SQLite reopening, score-cache/recall projection, verified BRD storage, context admission and reconstruction agree on failed outcomes. The final scene suite includes an 80-case gauge/auto-shift/authority matrix and real-worker cases with a pre-abort Hazard miss, pending note, exact accepted input and delayed rendering.

Independent review exposed three same-root-cause replay consumer gaps, all corrected and independently re-reviewed: new terminal-aware documents use extension schema 4 so the real previous reader rejects them while ordinary v3 remains readable; video export explicitly rejects aborted tracks before resource work; Watch honors the recorded terminal boundary even when a genuine preceding survival-failure event or chart deadline would otherwise finish early. Abort video export is intentionally unsupported, not claimed implemented. BRD envelope/reference codec identity remains version 3.

The controller freshly built `main` and 24 focused targets and passed all 24 CTest suites in 5.20 seconds. The actual previous-codec admission helper also passed in the implementer's final verification. Renderer/audio/OS ingress and wall-clock presentation remain fixture boundaries; this is not a full-device gameplay race certification.

### Course and chart preparation

Course continuation preserves real live gauge/combo carry across authored versus fallback LN modes. Course result capture records effective counts for both played stages and remaining entries without changing library metadata. Fresh retry, practice, viewer and ordinary launch paths normalize notes before immutable policy/provenance construction. DP-FLIP runs before independent P1/P2 options, while retained prepared charts are not transformed twice.

Real parser/scene-method fixtures cover authored LN carry, SQLite/BRD result persistence, preparation ordering, and asymmetric DP notes including scratches, long notes, invisible notes and mines. Independent review found a further same-root-cause partial-course Retry Same gap: the verified replay-to-live factory omitted saved FLIP/P2 fields. The corrected factory passes eight real persistence/reopen/runtime-consumer/restoration/continuation cases, including retained full-prefix pointer identity and truthful Verified aggregate provenance. A fresh independent scoped re-review approved that correction.

The controller freshly built `main` and all 19 gameplay/course/practice regression suites alongside six audio suites; all 25 pass in 6.49 seconds. Scene dispatch, jukebox resources and interactive input remain explicit fixture boundaries, not full-device session evidence.

### Downloaded archive expansion

Both miniz and libarchive now check declared and actual expanded bytes before writes: 2 GiB/member, 8 GiB/archive, 100,000 entries and a 256 MiB initial free-space reserve. Cancellation reaches entries and streaming chunks, including discarded libarchive payload. Existing owned-attempt cleanup handles failure without recursively deleting caller-owned output. Limits and manual handling for larger packages are documented in `docs/find-bms-archive-limits.md`.

Bounded real compressed fixtures reproduce misleading size metadata, unknown-size streams, budget/count failures, corruption and mid-member cancellation without exhausting storage. The actual download-attempt workflow fixture preserves real staging/extraction/cleanup; HTTP download alone is substituted with a local file copy. Independent source review approved the change. The controller's 24-suite verification includes the final strengthened partial-output cleanup fixture. Free-space snapshots are not reservations, and cancellation cannot interrupt an underlying decoder before its current read returns.

Final whole-branch review caught the remaining verification representation: packed inspection retained a batch of unchecked members, while extracted hashing allocated entire chart files and duplicated MD5 input. The real default packed/extracted paths each fail a pre-allocation guard at 17,825,792 bytes before the correction. Verification now pre-admits raw entries and chart sizes, reads one bounded member at a time, checks actual bytes and uses existing incremental SHA-256/MD5 with cancellation. Defaults are 16 MiB/BMS member, 256 MiB cumulative verification bytes, 100,000 inspected entries and 64 KiB application checkpoints. Budget, integrity and cancellation failures leave no pending artifact or committed output; genuine mismatch/hashless behavior and bounded codec/solid fallback remain supported.

The final follow-up passes the original guarded regression, real compressed/extracted workflow limits, post-sizing growth, calibrated read/hash cancellation, precommit cancellation, metadata filtering, codec fallback and owned-attempt cleanup preserving unrelated files. All nine focused suites pass; independent scoped review inspects all eleven corrected files and affected callers and approves both specification and quality. Archive metadata/index/decoder allocations and temporary vector growth remain distinct from the admitted payload bound; this is not an RSS or hard interruption guarantee.

### Android platform boundaries

Real archive-copy method fixtures verify repeated partial read/write/close/cancellation failures preserve unrelated files and remove owned output. Archive copies now have an 8 GiB ceiling and a 256 MiB current free-space reserve, without capping folder imports. Fresh host verification passes 17 release-workflow and 10 lifecycle Python tests, plus 39 Gradle JVM tests in each flavor.

The original Android HTTP failure was reproduced on an installed release build. Both corrected flavor APKs pass real target-process HTTP table/archive, HTTPS, upgrade and downgrade-rejection checks using bounded local servers. The XML configuration changes only cleartext admission for arbitrary public hosts, not TLS trust; HTTP remains observable/modifiable in transit. The platform default and opt-in are documented by [Android's network-security guidance](https://developer.android.com/privacy-and-security/security-config#CleartextTrafficPermitted).

On API29, actual DocumentsUI selection and delegated fixture grants pass through the production activity result handler. Real persisted URI access, synthetic-path listing and descriptor reads remain valid after a process restart. API28/29, API30 denied/granted and Play policy controls also pass in the actual-method host fixture. This does not claim a complete native Add Folder/SQLite library-index run.

The deployment helper's build-only action completed a full Firebase native release build. Subsequent Java/resource/test packaging deliberately reused its hash-verified native libraries in both flavors while another native correction was underway; it does not claim inclusion of those concurrent changes. An independent Android review approved the scoped fixes. This intermediate evidence is superseded for final compilation by the converged native/mobile gates below.

### Database startup integrity

A synthetic existing v0 score database reproduces the repeated chart attachment alias at version2 through public `EnsureSchema`, confirming DATA-01. LN and duration migration passes now reuse their connection's chart attachment with explicit acquisition ownership, preserving the enclosing atomic transaction. The freshly rebuilt full score-provenance database suite passes v0/v1/new-database upgrade and reopen, exact outcome preservation, and two same-connection caller rollback cycles. Independent review approved the frozen two-file correction.

The Android replay failure is independently classified as DATA-02: the actual frozen native library's fresh-schema initializer fails with `SQLITE_IOERR_GETTEMPPATH` (6410) while dropping `replay_lane_cover_events`. Both disposable internal and external databases reproduce it; only changing SQLite's temporary location to private cache makes each pass. A new Android Application initializer configures `SQLITE_TMPDIR` before SDL activity startup; both flavor APKs then pass the same native schema initializer without a test override. This is actual bundled SQLite execution, not Android's different platform SQLite. Independent review approved all four files. A separately created fresh user's normal SDL launch proceeds through result recovery and completed library refresh without the original profile failure; the headless Vulkan frame remains black, so menu rendering/navigation is not certified. That owned user and test APK were removed and the emulator stopped.

The initialization point precedes activities/services, while content providers are an explicit lifecycle exception; no application provider in this scope invokes bundled native SQLite. See [Android Application lifecycle](https://developer.android.com/reference/android/app/Application.html) and [SQLite temporary-file selection](https://www.sqlite.org/tempfiles.html). Neither global in-memory temp storage nor repeated mutation of `sqlite3_temp_directory` in production is used. Existing emulator user data was not cleared or inspected. Passing platform-boundary tests alone is not release-readiness approval.

### Lua coroutine transfers

The actual Lua host now copies against an immutable argument boundary and checks caller/coroutine capacity before argument and result transfers. Real document-loader and configured live-callback tests reproduce protected stack overflow on ordinary resume/wrap calls, then pass 20 bounded subprocess cases covering values, nils, yields, original error objects, oversized transfers and VM budgets. No native crash or hang was observed, so the finding is classified as P2 functional failure, not a demonstrated P1 crash. Independent review approved the isolated correction; a fresh controller main/focused build and all 17 Lua suites pass in 6.38 seconds. This is not mobile or full gameplay-session validation.

### Bounded chart audio

The shared renderer now rejects excessive duration and sound tails before dense allocation, caps cumulative mixing work, checks cancellation within mixing/output blocks, and converts PCM one block at a time. An exclusively owned staging directory and checked file close precede atomic publication; cancellation and errors preserve an existing destination. Actual renderer/parser/decoder/cache/adjacent-worker regressions pass normal PCM, playback-rate/full-tail controls, tiny budgets, overflow-scale metadata, mid-mix/partial-write cancellation and cleanup. Independent review approved the exact six-file correction; the fresh controller main/25-suite run passes in 6.49 seconds.

The 128 MiB float-payload ceiling permits approximately 6m20s at 44.1 kHz stereo; longer/heavier renders fail explicitly without blocking chart gameplay or silently truncating export. It is **not a total RSS or aggregate decoded-asset budget**. Transient old/new vector storage, decoded sounds and archive batches remain additional memory. Limits and tradeoffs are documented in `docs/chart-audio-render-limits.md`; no mobile memory or instrumented close-failure certification is claimed.

### Skin movie clock

The adapter now keeps its injected clock paused and seeks it to the normalized source timestamp before either starting/seeking or updating VideoPlayer. This gives initial presentation, delayed presentation, backwards seeks and wraps the same zero-origin convention without changing global player/stopwatch semantics.

The real adapter/FFmpeg/bgfx Noop fixture observes timestamp-coded decoded Y-plane uploads and actual drawable commit/submission, not optimistic player-position fields. Both immediate and delayed cases pass 80 frame observations over four loops (three wraps), plus held-source/backward/resume and budget/cancellation/texture-cleanup controls. Independent review approved the six-path correction. The controller freshly built main and passed all 15 movie/video/resource suites in 9.64 seconds; the worker also passed 40 repeated scenario executions. This is headless decode/upload evidence, not hardware-GPU screen pixels or mobile playback certification.

### Bounded duration graphs

Dense note distributions retain exact ordinary one-second bins up to their renderer-compatible ceilings (1638 normal bins including padding, 1637 dynamic bins). Longer distributions are explicitly omitted, not rescaled or truncated. Sampled skin gauge histories admit at most 4096 samples per channel; overflow clears the entire trace while preserving live properties. Full timelines, sparse BPM data and distant-note judgements remain intact. Result, replay, course and skin fallbacks retain omission instead of resurrecting misleading partial graphs. Aggregate durations and BPM offsets use checked conversions.

Durable gauge event capacity and overflow semantics are unchanged: only initial reservation is capped at 4096, and later actual events still grow normally. Course synthetic padding is preflighted before any fabricated tail is added. On the supported int/float ABI, raw chart-plus-dynamic display samples total at most 298,088 bytes; this is not a whole-model, process, snapshot or durable-history memory ceiling. The tradeoff is absent long-duration graphs, not rejected or shortened chart gameplay.

Allocation-guarded actual parser/model/selector/simulation/course/result fixtures safely reproduce baseline requests without allocating multiple gigabytes. Corrected late judgements, 4100 retained durable events, overflow controls, cancellation and stale-publication boundaries pass. Independent review approved all 23 files and their affected contracts; a fresh controller main/focused build and all 13 suites pass in 2.90 seconds. The guard covers ordinary C++ allocation requests, not all process allocators; selector barriers are not full UI scheduling or interruption-inside-model evidence.

### Metadata and iOS archive transport

Desktop metadata GET/POST now enforce a 16 MiB retained-body limit at the actual curl callback, before append and regardless of response status/declared size. Checked chunk arithmetic and explicit receive errors prevent partial success. Cancellation reaches all six worker lookup sites and idle curl progress. Existing mobile text APIs/checkpoints remain separate; no streamed mobile metadata cap or interruptible mobile POST is claimed.

The iOS archive caller now downloads to files with an 8 GiB admission limit. Completed/staged file sizes are independently checked; same-volume staging moves the file, while cross-volume fallback uses a 64 KiB checked/cancellable buffer. Only successful final publication atomically replaces the private attempt destination, preserving Google Drive's second download into that same path. The delegate owns abort/staging state; a synchronized progress gate detaches borrowed callback context before bridge return. Late callbacks cannot publish or recreate cleaned staging. Existing HTTP(S)/downgrade, authenticated IR and extraction contracts remain unchanged.

Actual callback and macOS Foundation regressions reproduce the original excessive retention and whole-file NSData reads with tiny payloads. Corrected real loopback GET/POST/NSURLSession tests pass, including exact/oversized/chunked/error responses, replacement, active/idle cancellation and checked copy/read/write/close/rename failures. Parent freshly built main and passed eight focused suites in 6.56 seconds; independent review approved all 15 files. All eight transport suites also pass in the final full desktop run.

This is macOS Foundation/source-extraction evidence, not full UIKit execution, iOS device memory profiling or a real Google Drive transaction. The OS may spool a chunk before delegate notification; the policy prevents oversized publication rather than claiming an exact OS-spool bound. Cancellation after the final check may lose to publication, and the full 190-second timeout was not exercised. See `docs/find-bms-transport-limits.md` for budgets and boundaries.

## Review coverage and limits

The frozen inventory assigns every one of 2,182 tracked baseline paths exactly once: gameplay/persistence/input 370, security/network/storage 141, rendering/audio/media/resources 373, platform/build/UI 330, supporting tests 476, supporting contracts 85, vendor integration 283, and binary integration 124.

Four independent reviewers completed risk-driven sweeps of the first-party slices and recorded differentiated coverage. Critical ownership, admission, persistence, cancellation, resource and caller boundaries received targeted implementation reading. Other files received interface/integration inspection or inventory only. The counts are assignments, **not a claim of line-by-line review or clearance of every implementation body**.

Generated parser/SQLite/dialog sources, submodule internals, vendored headers/libraries and binary assets were integration-only. This is not an upstream dependency/CVE audit, exhaustive schema or reference-engine equivalence proof, native-driver race certification, or universal device-memory benchmark. Frozen-baseline reviewers ran no builds/tests or network requests; correction-specific runtime evidence is recorded separately.

## Final integration gates

The first fresh all-target desktop build completes 1,743 compilation/link actions. Its full CTest run passes 347/350 and identifies three introduced test-integration gaps: two selector fixture families lack updated option-helper interfaces, and one result source guard expects the old unbounded padding spelling. Four test-only files correct these dependencies while preserving all existing lifecycle/ownership assertions and adding argument-propagation checks. Independent review approves the correction (`00b4c589`); no production behavior changes or additional major findings are attributed to it.

A fresh all-target rebuild and full parallel CTest then pass **350/350 in 89.08 seconds**, at `00b4c589`. No skipped test, swallowed failure, weakened allocation guard or whole-file formatting was used to obtain that result. That source also passes 66 release-critical native suites, 88 iOS Python checks, actual UIKit arm64 unsigned compilation and artifact audit, and a fresh Android Firebase native release helper build. These precede the final correction described next and will be repeated as required on the corrected source.

The final independent review reads all 114 then-changed files and affected cross-scope callers. Its sole remaining major reopens the already-assigned SEC-02 packed/extracted verification boundary, not a twenty-third finding. One cohesive correction and a fresh independent eleven-file scoped re-review close it with explicit specification/quality approval and no introduced major regression. The earlier Android packaging pipeline was intentionally stopped so old-source validation could not be mistaken for corrected-source evidence.

After that last source correction, a fresh all-target desktop rebuild completes 226 actions and full parallel CTest passes **352/352 in 98.90 seconds**. The repeated iOS release verifier passes **66 native suites in 19.90 seconds**, all **88 Python checks**, actual UIKit arm64 unsigned compilation and artifact audit. Source hashes match the independently reviewed correction; these gates include the final verification code, not only the earlier extraction change.

Both final Android release flavors build through `scripts/android_firebase_deploy.sh --build-only` with the current corrected native source, not the earlier frozen-JNI packaging shortcut. Firebase completes in 2 minutes 37 seconds; Play completes in 12 seconds, reusing the just-built native cache. Normal test-APK tasks pass in 6 seconds each. On the booted, unlocked API29 emulator, both installed target processes pass HTTP table/archive, HTTPS, HTTP→HTTPS, HTTPS→HTTP rejection and early private SQLite-temp configuration probes. No SDL/profile database is opened by this final instrumentation; the earlier bundled-SQLite startup proof remains separately scoped above.

Restoring the earlier Firebase APK after Play correctly triggers Android's version-downgrade rejection. A new build-only Firebase run uses its normal later automatic version code, succeeds in 11 seconds and passes the same target-process checks again. No version override, app-data clearing or signing workaround is used. The original emulator user's data remains intact; the test APK and port forwards are removed, Firebase is restored and the owned emulator is stopped.

The final code/test/build correction is committed as `ac9b48a9cb0f01a17bc5440c264d5f2d3e0c5007` (119 changed paths from the merged baseline). The gates use its byte-identical frozen source; the original full desktop/iOS/Android runs precede the bookkeeping commit, and the Firebase restoration build follows it. Source/evidence/artifact hashes and command logs are retained locally. The five supplied review documents still match their initial SHA-256 fingerprints and remain untracked; only this new report and its implementation plan are added. Generated parser and shader sources are unchanged. No manual distribution action or new worktree is part of this cycle; the new branch is not merged or pushed by this cycle.

## Decisions and tradeoffs

- Separate chart-verification budgets from storage admission: a 2 GiB extraction limit cannot safely bound retained hash-verification memory. Legitimate unusually large BMS files/packages can require manual handling under the documented 16 MiB/member and 256 MiB aggregate policy; audio/BGA storage limits are unchanged.
- Allow disjoint preparation but serialize native/CMake ownership: this avoids shared-build races and source/artifact ambiguity at the cost of a longer validation cycle. The graph policy separately omits overlong display graphs rather than truncating playable charts or durable scoring history.
