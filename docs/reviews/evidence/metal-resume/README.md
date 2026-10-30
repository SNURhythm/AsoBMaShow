# Native lifecycle timing capture check

This optional Simulator harness includes production `MetalDisplayLink.mm`, the
drawable-provider hook and lifecycle state machine. It renders a small clear
pass using real display-link callbacks, saves the initial session after 160
frames, then saves the resumed session after another 160 frames. UIKit supplies
the lifecycle events; they are not injected by the test driver. There is no
method swizzling or replacement clock.

Build without installing or distributing:

```sh
python3 docs/reviews/evidence/metal-clock/build_simulator.py \
  --resume-only --output /tmp/asobmashow-metal-resume-audit
```

Install on an already available Simulator, launch
`com.snurhythm.metal-resume-audit`, wait for `Documents/initial.json` in its data
container, launch Settings to background it, then relaunch the same running
audit app. `Documents/resumed.json` contains both attachment and lifecycle
histories. Rendering stops after the second phase or 90 seconds. Uninstall the
audit app afterward.

The checked results use the installed iOS 27.1 iPhone Duo Simulator. They verify
request/scheduled observations across suspension, retention of old sessions,
and bounded samples. An earlier instrumented run included a callback timeout
and automatic reattachment before suspension. That observation motivated
explicit detach-reason reporting. Retry polls include the existing one-second
cooldown and are not dropped-frame counts.

The retained [final result](results.json) contains two attachment sessions,
with lifecycle generations zero and one. The previous session retains 121
samples (120 initial plus one sparse sample); the resumed session retains 120.
All 241 sampled requests have scheduled-handler observations and none has a
late CPU request. The single retry poll coincides with lifecycle cancellation.
These are Simulator observations, not physical display measurements.

The Simulator SDK omits `MTLDrawable.presentedTime` and `addPresentedHandler`.
Display timestamps are explicitly unavailable. This validates instrumentation
and lifecycle bookkeeping, not physical latency, ProMotion, iPad compositor
eligibility or recovery to Direct mode.
