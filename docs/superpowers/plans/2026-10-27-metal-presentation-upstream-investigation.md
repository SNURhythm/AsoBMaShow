# Metal Presentation: Upstream Investigation and Validation Plan

> **Execution status (2026-10-27):** The user subsequently authorized the bgfx upgrade candidate, implementation, commit and push after the concurrent job finished. Lower-effort implementation subagents are also authorized. Device measurements and conditional display-link work remain separate from preparing the candidate; deployment has not been requested.

**Goal:** Determine whether newer bgfx can reduce the observed presentation delay before attempting another CAMetalDisplayLink integration, while preserving frame cadence, input/audio behavior, and supported platforms.

**Architecture:** First compare the current timer-based renderer with an upstream-only candidate, retaining the same rendering workload and three-drawable pool. Separately test the upstream GPU frame-limit fix. Revisit display-link scheduling only if measurements identify a remaining problem it can address.

**Tech stack:** C++23, Objective-C++, SDL, bgfx/metal-cpp, Metal HUD, Instruments, temporary diagnostic builds, CMake and native Metal tests.

**Spec:** The constraints and proposed design in this document supersede the implementation assumptions in the [historical display-link design](../specs/2026-10-27-metal-display-link-design.md). That experiment remains a historical record.

## Global constraints

- The initial investigation was planning only. That restriction was superseded by the execution authorization above. Work in the current checkout and serialize builds per build directory.
- No automatic worktree creation, branch switching, reset, stash, device installation, or deployment.
- The user permits temporary diagnostic builds measuring callback, GPU completion, and actual presentation timing. Remove the added diagnostics from the final build; merely leaving them behind a disabled runtime switch is insufficient.
- Retain the iOS 15 minimum, UIKit main-thread ownership, the existing application/render worker, independent realtime input/audio, export arbitration, and prepared-frame retirement.
- Keep three CAMetalLayer drawables for the initial experiments. Drawable-pool size, GPU work in flight, and frames waiting to be displayed are different quantities.
- Hold renderer optimizations, shaders, BGA, MSAA, render scale, frame cap, timer accounting, and bgfx flip mode constant within each comparison.
- No MTL4Compiler migration, precompiled-shader optimization, visual-time prediction, audio-offset adjustment, or general renderer rewrite in the latency experiment.

## Findings from the source investigation

### Revisions examined

- Current bgfx.cmake pin: `6e2138a92033af5f1e24d262448e3a81b987df10`.
- Current nested bgfx pin: `0b1c1ecaca917acc093f755bf8468711a5b24aee` (2026-03-01).
- Upstream bgfx.cmake: `5ae5fa5b121974b9de084539bb784d97fa4cd8a6` (2026-10-26).
- Its bgfx revision, also the upstream master examined: `fe9bbdec66110f8626b9251a4947fc336ac3f62c` (2026-10-26).
- Application source was inspected around `3409363b` and `1b88ab04`. The concurrent job advanced HEAD during the investigation. Neither revision is a frozen performance baseline: record the final application SHA and clean task state before any later A/B run.

The comparison used upstream source and commit patches without fetching into or changing the working submodules. No performance experiment was run.

### Upstream changes relevant to this issue

1. **The GPU frame limit is actually honored.** [526b886105dd / #3715](https://github.com/bkaradzic/bgfx/commit/526b886105dd8cca22d8a94e7c49f21e9a1e5af6) passes the configured `maxFrameLatency` into the Metal command queue, initializes its semaphore with that value, and updates full-drain accounting. Our version initializes the semaphore with `BGFX_CONFIG_MAX_FRAME_LATENCY`, which defaults to three, regardless of the requested value. Our app currently requests three on Apple platforms, so an unchanged upgrade still permits three. This fix creates a meaningful independent experiment with two GPU frames in flight while retaining three drawables. The semaphore is signaled on GPU command-buffer completion, not presentation; it cannot by itself guarantee a one-frame visible queue.

2. **Rendering and presentation are submitted separately.** [0158b15c2398 / #3714](https://github.com/bkaradzic/bgfx/commit/0158b15c2398f56aec1d99fa480eaf78745cd978) fixes a `BGFX_FRAME_FLUSH` freeze. It commits rendering at the end of `submit()` and uses a separate command buffer in `flip()` to present. Our version normally requests presentation and commits the rendering command buffer together in `flip()`. This is relevant to scheduling, but the upstream change is not a claimed fix for our display-link regression. Our iOS path already uses `BGFX_RESET_FLIP_AFTER_RENDER`, so the new structure does not establish that an entire application frame is saved.

3. **Empty frames avoid unnecessary drawable acquisition.** [ac4da6369a9b / #3748](https://github.com/bkaradzic/bgfx/commit/ac4da6369a9b710c95b25dd4f2aff983af51bd4b) avoids creating a backbuffer encoder merely for empty/debug processing. This may help maintenance, offscreen, or idle cases. Continuously rendered gameplay still needs a drawable; this is not a direct explanation of its sustained delay. Our `PresentationSchedule` already skips many idle submissions.

4. **Resource retention changed.** [227eac0d75de / #4060](https://github.com/bkaradzic/bgfx/commit/227eac0d75de005d5357ae0d2ecee1f0988ea6e1) uses command buffers with unretained references and explicit deferred resource release, including drawables. This can change CPU overhead and resource lifetimes. Treat both latency and safety as unmeasured, especially for our externally owned native video textures; do not transplant only the unretained-reference call.

5. **Some fixes are outside the reported iPad case.** The dynamic VSync change [4997ad09ed52 / #3709](https://github.com/bkaradzic/bgfx/commit/4997ad09ed524e0026ebec8a264ca309dea9a3e3) is guarded for macOS. Occluded-window handling is likewise not evidence of a foreground iOS gameplay fix.

Upstream still obtains ordinary drawables with `nextDrawable()` and uses command-buffer `presentDrawable()`. Its examined Metal backend contains no CAMetalDisplayLink integration or MTL4Compiler adoption. **An update is a worthwhile candidate to measure, not an established cure.**

### What the previous experiment establishes

The [Metal HUD follow-up](../../reviews/2026-10-27-metal-hud-followup.md) records user-reported rolling Present Delay values:

- Before the branch: approximately 16.00 ms.
- Display link enabled, `5804f12c`: approximately 23 ms.
- Activation disabled, `e6329d76`: 16.66 ms.
- Integration removed, `620a1046`: approximately 16.5 ms.
- Old timer accounting restored, `49e55e65`: still approximately 16.5 ms.
- After branch-added telemetry removal, `74215625`: approximately 16.15 ms.

These comparisons implicate the enabled display-link path, but do not isolate its cause. Device conditions and exact distributions were not independently recorded. The difference is not proof of exactly one extra 120 Hz refresh, nor a measurement of physical input latency.

The removed code already set `preferredFrameLatency = 1`, suppressed the timer wait for display-link frames, used the application worker, and retained at most one callback drawable. It is not useful to propose those same changes as a new fix. It checked `targetTimestamp` when accepting a callback but did not use the deadline later to explain a missed present. It did not retain `targetPresentationTimestamp` for analysis. Rendering continued after leaving the callback through the existing scene loop, with bgfx's GPU semaphore wait still inside `submit()`.

Pause/resume sometimes reduced delay with blur enabled. No existing observation proves whether queue occupancy, refresh phase, workload, or composition mode caused that change. Test that transition explicitly; do not attribute it to input or change pause behavior to force a lower HUD number.

## Decision and proposed design

Three approaches remain credible:

1. **Keep the current backend and isolate individual fixes.** Smallest compatibility change, but maintains local backend patches and may misrepresent upstream resource/queue interactions if a fix is only partly backported.
2. **Upgrade bgfx first, retain timer presentation, then evaluate its effective frame limit. Recommended first experiment.** Tests maintained upstream behavior before adding more local scheduling machinery. The cost is a real integration migration, not just changing a gitlink.
3. **Rebuild the CAMetalDisplayLink path.** Offers explicit display deadlines but does not resolve the known regression by itself. Defer until measurements justify a particular scheduling change; no callback-loop rewrite just to adopt the API.

This is a recommendation, not a user-approved implementation choice. If the upgrade is too broad to evaluate economically, a narrowly scoped semantic backport of #3715 is an alternative experiment, not an additional simultaneous change. It must include semaphore drain and resource-lifetime validation; changing only the initial semaphore count is insufficient.

### Why the HUD number alone cannot select the winner

[Apple defines Present Delay](https://developer.apple.com/documentation/xcode/understanding-metal-performance-hud-metrics) from the `presentDrawable` call to display, averaged over 120 frames. Upstream moves that call to a separate presentation command buffer after committing rendering. A shorter interval can therefore result from moving the measurement's starting point later, even if the pixels contain equally old game state.

Record actual presentation intervals and a stable CPU frame/input-snapshot reference as well as Present Delay. GPU completion is not display completion; `bgfx::frame()` returning is not either of them. The three-slot drawable pool is not a measurement of three frames of latency.

### Display-link contract questions to retain for later

The [current display-link documentation](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink) says to commit work before calling the drawable's `present()`. [The update deadline](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/update/targettimestamp) applies to that call; GPU execution may continue afterward. The [command-buffer convenience API](https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:)) defers the drawable's presentation request until the command buffer is scheduled.

However, Apple's downloadable [display-link sample](https://developer.apple.com/documentation/metal/achieving-smooth-frame-rates-with-a-metal-display-link), inspected in `Shared/Renderer.mm`, itself calls `presentDrawable` before `commit`. It also requests latency two rather than our previous one. Thus neither documentation wording nor the sample proves that our convenience call is incorrect. Measure whether its deferred request misses the callback deadline. A direct-present experiment must obey ordering for all drawable-writing command buffers and preserve lifetime until safe retirement. Do not add `waitUntilCompleted()` to force ordering, and do not use timed-present variants with a display-link drawable.

Other hypotheses remain: callback delivery/run-loop phase, time spent updating the scene before submission, waiting on bgfx's GPU semaphore after acquiring a callback drawable, actual display refresh differing from SDL's maximum-rate report, and retained/presented frames accumulating independently of the GPU semaphore. SDL obtains the iOS mode rate from `maximumFramesPerSecond`; the current pacer is rate-aware, not a measurement of instantaneous refresh deadlines.

## Review focus

- Lowering Present Delay by moving its start timestamp while increasing displayed state age: compare a stable CPU reference and actual presentation, Task 3.
- GPU frame-limit changes releasing transient/native video resources too early or deadlocking drain: ownership and export checks, Task 2.
- Pause/resume, idle, and refresh changes producing a good steady-state average but a bad first visible frame: separate transition measurements, Task 3.
- Diagnostic callbacks retaining drawables, allocating per frame, or outliving shutdown: bounded diagnostic lifetime and A/A checks, Task 1.
- A future display-link fallback calling `nextDrawable` while the layer remains attached to the link, or losing prepared work: explicit ownership transitions, Task 4.

## Tasks and decision gates

### Task 1: Freeze the comparison and establish trustworthy measurements

**Files:** Later create `docs/reviews/2026-10-27-metal-presentation-upstream-results.md` and an evidence directory beneath `docs/reviews/evidence/metal-presentation-upstream/`. Temporary code, if needed, belongs in dedicated `src/video/MetalPresentationDiagnostics.{h,mm}` plus narrow hooks in `src/main.cpp`, `cmake/BgfxMetalPresentation.cmake`, and the actual generated backend. Do not restore the old general-purpose telemetry changes wholesale.

**Interfaces:** Produce a run manifest and frame-correlated event records. Required manifest fields are application/dependency SHAs, diagnostic mode, device/OS, compiler/configuration, validation/HUD settings, chart segment, skin, BGA/blur, MSAA/scale, cap, requested frame limit, drawable count, actual refresh observations, Game Mode/composition mode, and thermal state.

- [ ] Wait for the concurrent job to finish before any source/build changes. Record the actual application baseline; do not build changing sources and call that an A/B control.
- [ ] Use Instruments/Metal HUD for the initial baseline. Add temporary diagnostics only for unresolved intervals. Observe worker wake, input/scene snapshot acquisition, drawable acquisition, GPU-semaphore wait, encoding completion, each render/present command-buffer commit/scheduled/GPU-completion time, presentation request, and actual `presentedTime`. Display-link records additionally carry callback delivery, `targetTimestamp`, `targetPresentationTimestamp`, and whether the deadline was missed.
- [ ] Correlate by application frame attempt, drawable identity, and command-buffer role. Upstream can have multiple command buffers per frame; do not assume the presentation buffer's GPU duration is the rendering duration. Count committed-but-not-completed separately from requested-but-not-presented frames.
- [ ] Keep Core Animation/Metal times in their documented common time domain. Explicitly validate mappings before combining SDL, `steady_clock`, native-input, or audio times. A CPU snapshot-acquisition timestamp measures software age, not physical touch-to-photon latency or the generation time of an older realtime snapshot.
- [ ] Use bounded preallocated event storage, no per-frame file I/O or blocking locks in realtime paths, and no extra drawable retain just for logging. Async handlers must not capture scene pointers or destroyed collectors. Record missing/zero presentation timestamps, dropped events, shutdown and unfinished frames explicitly rather than substituting callback arrival time or reporting zero latency.
- [ ] Test record correlation with out-of-order completions, missing presentation, overflow, and shutdown. Compare diagnostics off/on/off on the same control; an observable perturbation invalidates fine-grained latency conclusions until reduced or explained. Write results after capture, off the rendering path.

**Gate:** Usable repeatable baseline and trustworthy event relationships. If instrumentation materially changes the result, improve the measurement, not the renderer.

### Task 2: Prepare an upstream-only integration candidate

**Execution decision (2026-10-27):** Use wrapper `fb7480cbac04e58bd803fa80863250c47f2ba9b4` with bgfx `6d0d15f2a51a4072ea7e4fa476d1b188b47708d8` (August 22) for the first candidate. It contains #3715, #3714 and #3748 and still accepts the 96 committed version-11 shader containers. The August 23 raw-buffer change (#3920) requires version 12 for every backend, including DX11; October's shader compiler also changes the default Metal language profile. The newer dependency layout, shader migration and October unretained-resource changes are deferred. This candidate tests the earlier scheduling changes as a package, not all changes in October's latest bgfx.

**Files to audit:** `bgfx` gitlink; `CMakeLists.txt`; `cmake/IOSBgfxMetalCompatibility.cmake`, `cmake/IOSBgfxMainThreadLayer.cmake`, `cmake/BgfxMetalPresentation.cmake`, `cmake/BgfxMetalReadback.cmake`, `cmake/BgfxMetalVideoSurfaces.cmake`; `scripts/ios_init.sh`; native build/project paths; `src/main.cpp`, `src/video/SDLDisplayBackend.cpp`, exporter initialization; `shader_src/make.py`; relevant native Metal/build tests.

**Interfaces:** A build using the wrapper's pinned bgfx/bx/bimg versions, the same timer policy, `BGFX_RESET_FLIP_AFTER_RENDER`, GPU frame limit three, and drawable pool three. Record every necessary compatibility adaptation separately from optional optimizations.

- [ ] Inventory the migration before editing. Upstream `Init` now uses `swapChain` and a separate `reset`; our call sites use `resolution`. The Metal implementation moved from Objective-C++ `renderer_mtl.mm` to metal-cpp `renderer_mtl.cpp`, invalidating literal generated-patch anchors.
- [ ] Resolve source paths deliberately. New bgfx.cmake uses FetchContent and `cmake/dependencies.cmake`, may ignore leftover nested submodule checkouts, and can fetch full histories. Preserve existing data/builds; do not delete old directories to silence a warning. Validate Xcode, headers, libraries, shader tools, desktop and Android paths against the resolved dependencies and disk budget.
- [ ] For each local patch, classify it as still needed, supplied upstream, or requiring adaptation. Preserve iOS main-thread layer mutations, legacy reflection compatibility, readback correctness, native video-surface ownership, and three drawables. Do not drop a patch merely because its text no longer matches. Verify supported Apple OS floors and cross-platform APIs before choosing this revision for production.
- [ ] Verify shader binary compatibility with the selected runtime/compiler. Regenerate only as required for compatibility; do not simultaneously switch to precompiled metallibs or change shader optimizations. If a shader/tool change is unavoidable, record it as a confound and use a narrower backend experiment if causal attribution is needed.
- [ ] Exercise GPU-completion resource retirement with one/two/three configured in-flight limits in small native fixtures, including transient buffers, borrowed NV12 textures, resize, empty/discarded frames, readback/export, and shutdown. Keep the visible drawable pool at three; a limit of one is diagnostic coverage, not a proposed production default.
- [ ] Build and run the affected groups after exclusive build-directory access is available. Confirm the timer-only candidate renders identically and captures the requested queue limit. No display-link code is enabled in this task.

**Gate:** A correct upgrade candidate with documented compatibility work. If this becomes a broad renderer migration, stop the latency experiment here and split the dependency upgrade into its own reviewed task; do not quietly expand this plan.

### Task 3: Test whether the upgrade solves the observed problem

**Interfaces:** Compare three named configurations with identical application/rendering behavior: A = current bgfx, timer, GPU limit three; B = upstream bgfx, timer, GPU limit three; C = upstream bgfx, timer, GPU limit two. All retain three drawables. B versus C isolates the newly effective frame-limit control; A versus B measures the upgrade as a package, not one upstream commit.

- [ ] Start on the affected physical M5 iPad, using the same chart segment and settings as the original report where recoverable. Record unavailable historical details. Use fresh baseline data, not 16 ms as a universal target. Verify 120 Hz during the run; test 60 Hz separately.
- [ ] Warm assets/shaders consistently. Run at least three paired steady-state comparisons per initial condition, alternating order (A/B, B/A, A/B; then B/C similarly). Capture at least 30 seconds after a fixed 10-second warmup. Keep capture modes equivalent, with no simultaneous builds or heavy background jobs. Record frame distributions and worst stalls; a single HUD screenshot is insufficient.
- [ ] First compare continuous gameplay with blur off and then blur on. Only expand successful candidates to custom skin, moving BGA, idle menu, cap 60/30, and an arbitrary cap such as 75. Separate pause/resume and background/foreground captures from steady-state aggregates; record the first resumed frame and whether a changed latency state persists.
- [ ] Compare Present Delay alongside actual present-to-present intervals, missed-refresh frequency, CPU snapshot-to-presentation age, GPU scheduling delay and time, worker/main input service, and audio underflow/keysound behavior. Use source/input timestamps only where a known event is correlated with a frame that reflects it. Do not replace judgment/audio clocks with predicted presentation time.
- [ ] Before seeing candidate results, register tolerances using A/A repeatability. Proposed initial guardrails: no reproducible increase exceeding 1 ms in median or p95 software state age or Present Delay, no more than 0.5 percentage-point increase in missed-refresh rate, no new whole-refresh transition penalty, and no correctness/input/audio regression. These are proposed experiment criteria, not established device guarantees. If baseline variability prevents those decisions, report inconclusive rather than relaxing tolerances afterward.
- [ ] Call the upgrade a presentation improvement only if the benefit exceeds baseline variation and is supported by stable-reference timing or cadence as well as the HUD. Call the historical display-link regression fixed only after a separate controlled comparison with display-link enabled; a better timer baseline cannot prove that. Repeat the winning case with all temporary diagnostics removed and capture externally with equivalent HUD settings.

**Decision:** If B or C provides sufficient real improvement, retain timer presentation and defer CAMetalDisplayLink. If only HUD delay decreases because the request moves later, report that limitation. If the upgrade is neutral or worse, retain A for the latency task and investigate a measured cause; do not restore the display link by default.

### Task 4: Only if needed, isolate the display-link regression

**Files:** Conditional future work in `src/video/MetalDisplayLink.{h,mm}`, a small pure lease/timing policy, `cmake/BgfxMetalPresentation.cmake`, `src/main.cpp`, `src/platform/IOSApplicationRuntime.mm`, `src/video/RendererAccessCoordinator.cpp`, and focused fixtures. Select the baseline backend from Task 3 first; do not change it during this experiment.

**Interfaces:** A frame lease must distinguish ordinary fallback, managed drawable, and managed rejection. It carries frame/geometry/lifecycle generation, callback/deadline/target-presentation times, and at most one retained drawable. Presentation policy and callback-delivery topology must be independently selectable in diagnostic builds so only one changes per comparison.

- [ ] Reproduce the old behavior on the chosen backend as a disabled-by-default diagnostic mode, preserving its one-frame latency request and no-extra-sleep policy. Keep unrelated branch telemetry and rendering changes out. Confirm whether the old delay regression still occurs.
- [ ] Use the measured timeline to choose one experiment: presentation request scheduling, worker callback handoff, or waiting for GPU capacity after callback acquisition. Do not change all three. Compare scheduled-convenience presentation with properly ordered direct presentation only if deadlines implicate that boundary. Verify the actual request timing, image correctness, and safe lifetimes under Metal validation before measuring with validation off.
- [ ] Keep lifecycle, exports and idle behavior explicit: one owner per drawable, no second acquisition for a rejected managed frame, no accumulation of pending callbacks, and no `nextDrawable` while a link still owns the layer. A paused link is not assumed detached. Invalidate before ordinary fallback/export, including inline photo export and queued owner work. Release unused leases on every early return.
- [ ] Reject stale generations and size/format mismatches without starving service work. Distinguish a callback already expired on arrival from a valid frame that overruns later; count both. Overload must not cause endless dropping or a fallback/link recreation loop. Retire previously prepared bgfx work through the existing `IOSPreparedFrame` rules.
- [ ] Add deterministic policy tests for duplicate callbacks/claims, missed deadlines, continuous overload, resize, cap changes, exports, quit and inactive intervals. Add a real native fixture verifying single acquisition, single presentation, command ordering, and restoration to timer mode. Include first-frame behavior after idle and pause.
- [ ] Repeat Task 3's acceptance comparison on the same physical device. Expand to another 60 Hz device and supported OS coverage before enabling by default. macOS can host contract tests but is not a substitute for iOS presentation measurements; iOS 15–16 retains the existing path.

**Gate:** Choose a production architecture only after a specific experiment improves the measured problem. If none does, the correct outcome is to keep the timer path and retain the evidence.

### Task 5: Verification and diagnostic removal

- [ ] Use focused builds first, combined in a single invocation per build directory:

  ```sh
  cmake --build cmake-build-debug --target tests_rendering tests_video tests_platform_ios -j 6
  ctest --test-dir cmake-build-debug -L '^(area_rendering|area_video|platform_ios)$' --output-on-failure --no-tests=error -j 6
  python3 tests/ios_build_setup_tests.py
  cmake --build cmake-build-debug --target main -j 6
  scripts/ios_firebase_deploy.sh --build-only
  ```

- [ ] A full dependency upgrade has broad impact: after focused fixes, run the full desktop suite and the Android release compile check with existing signing configuration, plus supported Apple target smoke checks. Use `scripts/android_firebase_deploy.sh --build-only`; no upload is authorized. Add new tests to `cmake/TestGroupCatalog.cmake`; avoid sleep-based timing assertions and broad new fixtures.
- [ ] Remove temporary source hooks, collection modules, diagnostic mode selectors, extra callbacks, compiler flags and test-only instrumentation wiring from the final candidate. Keep test fixtures and recorded evidence as appropriate. Verify no diagnostic object or callback remains in the production generated backend, including when telemetry is off; preserve unrelated pre-existing telemetry.
- [ ] Repeat the winning physical-device comparison without the added diagnostics, and inspect the final diff and resource lifetime behavior. Report measurements, regressions, uncertainties, and unsupported/unavailable device coverage honestly. A passing native test or simulator build does not prove a latency improvement.

Writing the initial plan performed no implementation or measurements. The subsequent candidate integration does not establish a device latency improvement; record its compatibility changes and available verification before physical-device comparisons.
