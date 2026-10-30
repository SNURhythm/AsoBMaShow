# Technical handoff: persistent portrait composition with CAMetalDisplayLink

Draft only. Not submitted to Apple or another service. This is an unresolved
presentation investigation, not a claim of a confirmed operating-system defect.

## Reported environment and question

The tester reports an 11-inch M5 iPad Pro on iPadOS 27.2. The device enumerated
locally as iPad17,2. On the original app, Metal HUD showed Composited in portrait
and Direct in landscape after rotation settled. Reported native drawable sizes
were 1668×2420 and 2420×1668 with Display Zoom disabled.

The application must retain CAMetalDisplayLink, native rendering resolution,
its existing rate request, preferredFrameLatency=1, and three drawables. The
question is which unmet integration condition or system presentation decision
explains the orientation split. We do not assume public APIs guarantee Direct
presentation in every fullscreen configuration.

## Evidence already collected

- A pure UIKit/CADisplayLink triangle probe was Direct in both orientations.
  This establishes that portrait Direct is possible on this device, but this
  probe did not use CAMetalDisplayLink.
- A CADisplayLink triangle probe on the SDL-created Metal surface was also
  Direct in both orientations.
- A CAMetalDisplayLink triangle probe on that SDL surface was Composited in
  portrait before bgfx initialized. It used update.drawable, latency one,
  main-thread rendering and command-buffer presentDrawable followed by commit.
  SDL's implicit high-refresh CADisplayLink was also active in this experiment.
- Older portrait app observations became Direct after a background interval
  with either Linear or Nearest layer magnification. Manual foreground
  recreation of the Metal link did not help. Backgrounding changes more than
  link lifetime, so it is not an isolated cause.
- The captured layer models had matching steady properties apart from
  orientation: fullscreen geometry, opacity, BGRA8/sRGB, identity transforms,
  three drawables, no mask and no animations. Model snapshots do not reveal the
  system's scanout eligibility reason.
- Nearest → Linear → Nearest reversibly changed landscape composition with
  Display Zoom enabled. That separate filter issue was corrected, but is not
  a complete portrait explanation.
- Build 202610301045 (`2c868e6f`) fixed a reproduced resize/MSAA attachment race
  and selected Linear at iOS layer initialization. The user reported portrait
  still Composited.
- Build 202610301116 (`552f60b2`) also suppressed SDL's implicit refresh clock
  while the Metal link was attached. The user again reported portrait
  Composited. No fresh device trace or installed-build identity accompanies
  that short follow-up report.

## What local verification establishes

Native Metal validation reproduces the old attachment mismatch and passes after
repair. Simulator tests using actual callbacks confirm that the extra SDL clock
can be stopped while Metal rendering continues, and restored for timer fallback.
The simulated 120 Hz capability exercises SDL's branch; it is not a hardware
120 Hz or latency measurement.

The production Metal-link delegate/provider also rendered 120 frames in each
of two small Simulator runs with normal UIKit and app-style nested main-loop
pumping. Layer-position mutations reached the presentation tree, and the
observed scheduled handlers preceded their update deadlines. This does not
prove physical iPad presentation timing or exclude every full-app timing issue.

Source review found no extra unconditional bgfx frame before presentation, no
illegal nextDrawable acquisition while the link is attached, and no SDL clock
recreation during ordinary rotation. Apple's downloaded CAMetalDisplayLink
sample also uses command-buffer presentDrawable followed by commit. These checks
do not identify the compositor's orientation-dependent decision.

## Remaining boundary

There is not yet a physical-device result for a pure UIKit **CAMetalDisplayLink**
case independent of SDL, nor a capture explaining the compositor's eligibility
decision on the failing build. The current source audit and Simulator cannot
supply those facts. Do not label the existing CADisplayLink-native result as
that missing control, infer an OS bug from elimination alone, or manufacture
certainty by changing more layer properties.

Any platform investigation would need to distinguish the scheduling/drawable
path, SDL/scene integration and system presentation state. No new manual test
or additional speculative deployment is requested as part of this handoff.

The detailed source references, sample provenance, native reproducer and
Simulator harnesses are in the [source audit](2026-10-30-ios-metal-source-audit.md).
