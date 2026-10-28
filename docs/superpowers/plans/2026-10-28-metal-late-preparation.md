# Later Metal frame preparation experiment

**Rejected and reverted.** The user rejected deliberate pre-input waiting for this rhythm game. Immediate input/scene preparation is restored; timing diagnostics remain. The following describes the historical experiment, not current behavior. Lower preparation-to-display age did not establish lower input or audio latency, and capture 24 exposed additional deadline crossings.

The user approved the next controlled change in the capture-23 analysis. Move input/scene preparation later; preserve presentation targets, three drawables, GPU limit three, and fallback behavior. No deployment.

Implementation choices: gameplay only; 30 submitted-frame warm-up; maximum CPU preparation-to-presentation-request duration in the last 120 submitted frames, with a 3 ms floor and 1.5 ms scheduling/wakeup margin. Delay at most 4 ms or half the refresh interval, whichever is smaller. A large workload immediately removes delay until it ages out. Reset on idle/missing submission, gameplay transitions, lifecycle/layer/rate changes and callback gaps over 100 ms. Cancellable sleeps are at most 1 ms each. Never begin preparation after a known expired deadline; do not wait for GPU/display completion. Preserve timing-v1 fields for comparison.

- [x] Test deterministic budget warm-up, workload spikes/rollover, resets, bounded delay, invalid times and cancellation/oversleep.
- [x] Integrate worker-owned budget with native lease and synchronous presentation-request observation; mark preparation after the wait.
- [x] Run focused/native rendering tests, desktop/iOS compile checks, independent review; document experiment; commit and push.

Success on device requires lower preparation-to-display age without worse cadence, deadline misses or missing display records. Local tests cannot establish that benefit.

Validation: 63/63 rendering tests; rejection/event-drain source contract; desktop main and unsigned iOS builds passed. Independent review found event-drain starvation; rejection now retires the lease then drains events before skipping update/render. Warm-up boundary and half-interval coverage were extended. Generation/rate reset wiring was inspected; native callback-driven transitions and latency still require device validation.
