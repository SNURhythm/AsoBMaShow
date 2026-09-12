# Visual catch-up decoder batching

## Approval and scope

The user approved batching visual catch-up operations and requested autonomous execution without further approval pauses on 2026-09-09. This changes decoder admission during an existing synchronous operation, not the audio scheduler or the readiness policy. Work stays on the current checkout and branch; no commit, push, worktree, or distribution is part of this request.

## Evidence

Baseline source: `267ff6974a1ab579619162a9a41b086ad2f4f03d`.

A generated 30-second H.264 clip (640×360, 30 fps, GOP 60), four independently preloaded video IDs, and 1,000 BGA events preceding a seek to 20 seconds reproduce a substantial synchronous stall. The median of seven retained samples after one warmup is 643.950 ms using unmodified production VideoPlayer objects. Instrumented runs observe exactly 1,000 successful seeks and flushes, with a median of 638.349 ms. Four events take 0.533 ms; 100 events take 64.098 ms in the instrumented baseline.

Sampling the uninstrumented workload attributes 1,723 of 2,184 main-thread samples to waiting for VideoPlayer's video mutex during catch-up. Decoder workers repeatedly decode intermediate positions that subsequent history events immediately invalidate. This is a synthetic desktop, debug-build, fake-audio-device, Noop-renderer benchmark with real FFmpeg decoding, not a mobile frame-rate claim. Physical cold-cache behavior and real audio/input jitter have not been measured.

Large-library probes show busy parsing workers rather than evidence justifying a global pool rewrite. Selection graph construction also has measurable waste, but is a separate subsystem and not part of this first change.

## Chosen approach

Add a scoped decoder batch to VideoPlayer. Entering a batch prevents new decoder actions and waits for an already admitted decoder action to leave the video mutex. The guard does not hold that mutex while callers execute seeks. Nested scopes remain blocked until the outermost scope exits. Exiting the final scope wakes the decoder without overwriting independent application suspension state.

Jukebox uses these scopes only when a catch-up operation contains multiple due events. It gates only the video players touched by that operation, holds their owning table stable for the scopes' lifetime, executes the existing base-then-layer sequence, and releases the scopes after the last command. A shared locked activation helper avoids recursive table locking. Zero-event and single-event progression retain a cheap path without per-frame batching allocations or scanning every loaded video.

All existing seek, flush, play, cursor, active-ID and failed-seek behavior remains in sequence. The implementation must not discard intermediate events, merge independent IDs that reference the same file, or claim to reduce the number of seeks. The worker admission check must be repeated while holding the video mutex, not merely before acquiring it.

## Alternatives

- Coalesce to the last event per ID: potentially larger savings, but an unsuccessful final seek can depend on an earlier successful seek. Changing that failure contract is not justified for this optimization.
- Replace all background workers with a global scheduler: broader risk to audio/input and no supporting measurement.

## Invariants and errors

- Keep audio-clock authority, BGA offsets, rate behavior, base/layer ordering, missing-asset fallback, and backward-seek reset behavior.
- Keep all referenced visuals eagerly ready before playback. Activation performs no new filesystem/archive reads or decoder initialization.
- Preserve independent suspension/disabled state when entering and exiting a batch, including nested scopes and exception unwinding.
- Guard destruction precedes player destruction and table-lock release. Do not introduce a video-table/position/image lock-order inversion or wait for a worker while holding a lock the worker needs.
- EOF, generation invalidation, cancellation, and shutdown must still wake and terminate. No polling loop replaces the existing condition variables.
- Preserve every existing failed seek's effects and subsequent command sequence; decoder scheduling between commands is intentionally changed.
- Add no dependencies, parser changes, global pool, production-only-for-test accessors, or whole-file formatting.

## Verification and acceptance

Use test-only FFmpeg boundary instrumentation in the established fixture translation-unit pattern. A test-only observer may also wrap actual condition-variable wait predicates to obtain a positive rejected-admission witness, while preserving the real wait and predicate behavior. Exercise actual VideoPlayer and Jukebox behavior, rather than source-text assertions or absence timeouts. Record a behavioral RED before the production fix. Deterministically check worker admission around a multi-event batch, seek/flush ordering including an injected failure, nested batch lifetime, suspension composition, and eventual decoding after release. Frame-marker fixtures verify final source-time selection. Cover shared base/layer IDs, different IDs sharing a file, missing IDs, backward/rapid seeks, zero/single-event paths, EOF and shutdown.

Repeat the same generated 4-ID workloads with 4, 100 and 1,000 events after the change, and run an uninstrumented control. Retain every command and raw sample. Target at least a 50% reduction in the 1,000-event median with no more than 1 ms absolute regression in the small case. These are investigation acceptance criteria, not flaky wall-clock CTest assertions. Keep timing, command counts, and frame correctness separate. If the target is missed, investigate and revise the smallest responsible design rather than claiming success.

Run focused media/Jukebox tests, the desktop main build and parallel full CTest. Verify shared native changes with the non-distributing iOS release verification and Android build-only workflows where locally available. Report limits and any unrelated failures explicitly.
