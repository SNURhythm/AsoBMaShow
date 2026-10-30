# Portrait recovery after backgrounding: measure the scheduling change

The user reports that portrait sometimes changes from Composited to Direct
after backgrounding and returning. Manual link recreation did not reproduce a
reliable fix. Timing or system presentation-state changes are plausible; this
does not identify which state changed or establish an OS defect.

## What the source does

SDL maps `sceneWillResignActive` to `SDL_EVENT_WILL_ENTER_BACKGROUND` and
`sceneDidBecomeActive` to `SDL_EVENT_DID_ENTER_FOREGROUND`.
`IOSApplicationLifecycle` blocks presentation at inactivity and does not reopen
it merely on `WILL_ENTER_FOREGROUND`. A generation change rejects old leases.
The worker invalidates its CAMetalDisplayLink while inactive. On return it
resets timer pacers, restores viewport state, retires any interrupted prepared
frame, and attaches a new link. A normal foreground callback does not replace
the SDL Metal view or its layer. UIKit's own scene and compositor transition
are additional state changes beyond a manual link restart.

These paths have no explicit Direct-mode request, extra resume delay, or
portrait-specific pacing calculation. `sceneDidBecomeActive` is a lifecycle
boundary, not evidence that every system transition animation has finished.
No app-side trace proves that an animation is the cause either.

## Three timing explanations

1. **Late submission:** preparation or queue scheduling misses the submission
   deadline. A late scheduled-handler observation is only an upper bound; the
   handler can itself be delivered after scheduling.
2. **Different assigned target:** the system changes the interval between the
   callback and `targetPresentationTimestamp`. Frames can meet their deadlines
   while the assigned presentation target remains later.
3. **Display after the target:** submission is timely but `presentedTime` is
   later than the target. This still does not isolate GPU readiness from system
   presentation policy without a GPU trace.

Earlier device captures 23 and 26 met their targets during steady gameplay.
They are not a matched Composited/Direct pair from this build and cannot settle
the current question. Late rendering is not an established explanation.

Apple defines the [submission deadline](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/update/targettimestamp)
separately from the [estimated display target](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/update/targetpresentationtimestamp).
The drawable's [presentedTime](https://developer.apple.com/documentation/metal/mtldrawable/presentedtime)
reports display time, with zero for an unpresented/dropped frame. The
[scheduled handler](https://developer.apple.com/documentation/metal/mtlcommandbuffer/addscheduledhandler(_:))
is a CPU callback after scheduling, not an exact hardware timestamp.

## V8 captures the comparison automatically

The diagnostic branch retains four attachment sessions, with lifecycle
generation, attachment/detachment times and detach reason. It records the first
120 callbacks in each session, then at most one sample per second in a 16-sample
ring. A separate 16-event lifecycle history uses the same `CACurrentMediaTime`
clock. Timeout reattachments can be distinguished from foreground reattachments
and explicit diagnostic recreation.

Copy Metal Presentation Diagnostics includes this history. `startup` and
`recent` statistics are separate. `targetLeadMs`, `targetAfterDeadlineMs`,
`requestMarginMs`, `scheduledObservedMarginMs`, and `displayDeltaMs` separate the
explanations above. Raw first/latest samples allow lifecycle alignment. Counts
distinguish callbacks without submission, pending observations, unavailable/zero
display timestamps, and actual displayed frames. `rateAtAttach` and
`drawableAtAttach` are attachment metadata, not continuous geometry/rate tracking.

Observers hold weak references to numeric samples, never retain a drawable,
layer or command buffer, and add no waits or file writes in the frame loop.
There is bounded diagnostic overhead; zero measurement overhead is not claimed.
Frame cap, preferred latency one, drawable pool three, native resolution,
CAMetalDisplayLink and presentation order are preserved. This is an evidence
build, not a claimed compositing fix.

## Verification and interpretation

Focused tests cover bounded history, startup retention, missing display-time
classification, lifecycle activation boundaries, and the generated bgfx hook
observing the real presentation command buffer. A standalone Simulator harness
uses real callbacks and actual UIKit background/foreground events; see
[the harness and results](evidence/metal-resume/README.md).

Keep orientation, zoom and rate fixed across a comparison. Record the HUD mode
before backgrounding; when it returns Direct, copy v8 diagnostics soon afterward.
The previous session is retained. The HUD label remains manual: these APIs do
not expose Direct/Composited status. If target lead, deadline margins and target
misses are alike in both modes, simple frame lateness is weakened and a system
eligibility/state transition remains a hypothesis. No arbitrary delay or forced
flush is justified yet.

## Physical v8 result: Composited → Composited → Direct

The user supplied commit `24e4e601` on iPadOS 27.2 and confirmed the HUD sequence
for all three attachment sessions. This is now a mode-labelled comparison,
although submitted-frame counts differ and the first session is mostly idle.

The first 120 callbacks in each session show:

| Session / HUD | Target lead median / p95 | Late CPU requests / sampled requests | Display >1 ms late / observed displays |
| --- | --- | --- | --- |
| Initial / Composited | 16.640 / 24.977 ms | 0 / 1 | 1 / 1 |
| First return / Composited | 16.602 / 24.946 ms | 0 / 21 | 0 / 17 |
| Second return / Direct | 16.608 / 16.650 ms | 1 / 33 | 1 / 30 |

The first return has four zero/unavailable display observations, and the second
has three; those are excluded from the displayed-frame denominator. Later sparse
samples add one on-time display to each Composited session. No presentation
callback is the same as a rendered frame: most idle callbacks have no submission.

Median target lead differs by only 0.006 ms between the two returns. Observed
display-versus-target medians are +0.011 ms and +0.002 ms. This weakens persistent
deadline misses or a persistent extra assigned refresh as the explanation for
Composited mode. Direct still contains a late request, so a late frame does not
automatically imply a persistent Composited state. Direct has fewer long-target
startup callbacks, however; a transition-time scheduling/state effect remains
possible. These measurements do not expose the compositor's eligibility reason.

Median request-to-display falls from 16.421 to 14.514 ms, while median
callback-to-request rises from 0.221 to 2.161 ms. That is consistent with the
CPU request occurring later relative to an almost unchanged target; it is not
proof of reduced input-to-display latency. These medians also cover different
sample sets and must not be treated as an exact per-frame decomposition.

The inactive intervals were 7.451 and 21.517 seconds. Link attachment followed
`didBecomeActive` by approximately 13 and 19 ms. One successful longer interval
does not establish a required delay or recovery threshold.

### A concrete foreground sizing instability

Session 2 attached at 1667×2420; sessions 1 and 3 attached at 1668×2420. The final
layer was 1668×2420. Attachment metadata alone cannot establish how long the
intermediate extent lasted or the dimensions of each submitted drawable.

The source explains and reproduces the intermediate width. The app averaged
`1668/970` and `2420/1408` into `contentsScale=1.719168814433`. SDL then recomputed
pixels during any `layoutSubviews`, including unchanged foreground restoration:
970×scale = 1667.59375 and 1408×scale = 2420.58969. The actual Simulator Metal
layer canonicalized that to 1667×2420, reproducing the width seen in the capture.
Foreground restoration schedules layout immediately and again after 0.12 and
0.35 seconds, so the app's exact pixel choice could be overwritten repeatedly.

The fix keeps SDL/UIKit's native contents scale and makes SDL's Metal view
recalculate pixels only when its bounds size or contents scale really changes.
An explicit renderer extent therefore survives unchanged layouts, while genuine
resize/scale changes retain automatic sizing and their resize event. The native
[regression harness](evidence/metal-sizing/README.md) failed on the old code and
passes the corrected behavior. This repairs the verified sizing instability;
whether it changes physical iPad Direct-mode recovery still requires the device
result. CAMetalDisplayLink, frame cap, latency request, pool size and timing
capture are unchanged.
