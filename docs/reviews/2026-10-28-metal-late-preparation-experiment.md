# Later Metal preparation: controlled device experiment

Capture 23 showed steady gameplay meeting its assigned display targets, with
about 16.61 ms between preparation and display. This candidate moves preparation
closer to the submission deadline. It does not change the assigned target,
command-buffer presentation, preferred latency one, three drawables, or GPU limit
three. It is an experiment; no device benefit is established yet.

## Policy

Only an accepted gameplay display-link lease can wait. The first 30 submitted
frames warm the estimator. Subsequent frames reserve the maximum observed CPU
preparation-to-presentation-request cost from the last 120 submitted frames,
with a 3 ms floor, plus 1.5 ms for command scheduling and wakeup variability.
The additional delay is capped at 4 ms and half the refresh interval. The largest
recent cost ages out after 120 further submissions. This is a bounded rolling
maximum, not an average or a fixed sleep.

Idle/missing submission, gameplay transitions, lifecycle/layer/rate changes and
callback gaps over 100 ms reset the estimator. A workload spike increases the
budget immediately; a long interruption instead starts warm-up again. Measurement
uses worker-owned synchronous state, independently of the best-effort diagnostic
capture. No asynchronous callback accesses the estimator.

Sleep requests are at most 1 ms each. The worker checks current lifecycle,
export and quit state between slices. An expired deadline or cancelled wait
rejects the lease; the main loop retires it immediately, still drains SDL/native
UI events, and skips update/render afterward. Legacy input edges and elapsed
scene time remain pending until an actual scene update. This prevents repeated
rejected leases from starving lifecycle or input processing.

No estimator can anticipate a new CPU spike, and an OS sleep can overshoot its
requested duration. The floor and margin are conservative starting choices,
not guarantees. The margin includes scheduling variability but is not adjusted
from asynchronous scheduled-handler observations in this version.

## Device comparison

Use the same chart, cap, and conditions as capture 23. Play for 15–30 seconds,
return promptly to the main menu, and use Parsing Logs → Export Log. The timing-v1
schema remains compatible; `preparation_start` now occurs after any deliberate
wait. Its distance from `callback` exposes the actual added delay.

Compare steady gameplay preparation-to-display age, actual display minus target,
consecutive display intervals, deadline margins, and submitted frames without a
display timestamp. Report warm-up, transitions and stalls separately. A smaller
HUD Present Delay alone is insufficient; the intended improvement is newer
input/scene state reaching the same display slot without worse cadence.

## Verification

The policy tests use injected clocks/sleeps for warm-up, 29→30 activation,
rolling spikes, invalid samples, resets, half-interval/4 ms caps, cancellation
and oversleep. Native Metal coverage verifies CPU cost observation and rejected
preparation on a cancelled lease. The iOS source-flow contract guards event-drain
and legacy-input-publication ordering; it failed before the rejection-path fix
and passes afterward. Native callback delivery and scheduler precision still
require a device run.

Desktop main and unsigned iOS builds passed, as did all 63 rendering checks and the iOS event-drain contract. Independent review caught the rejection-path event starvation, which was fixed before completion. Generation/rate reset wiring was inspected but not driven through native display-link callbacks in the headless fixture. No deployment was performed.
