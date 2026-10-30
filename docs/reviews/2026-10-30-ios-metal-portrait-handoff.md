# Technical handoff: persistent portrait composition with CAMetalDisplayLink

Draft only. Not submitted to Apple or another service. This is an unresolved
presentation investigation, not a claim of a confirmed operating-system defect.

## Latest follow-up

Build `202610301218` still has both reported symptoms. Its v6 snapshot confirms
native 1668×2420 drawables and an accepted orientation lock, but the scene is
portrait with a Landscape hint. The
[iPhone/iPad source comparison](2026-10-30-ios-ipados-presentation-differences.md)
records the updated evidence and separates the orientation-policy mismatch from
the unresolved compositor decision. Older pending-control statements below
refer to the earlier stages of this investigation.

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

- The user reports an iPhone 16 Pro on iOS 27.2 running at 120 FPS was Direct
  in both orientations even before this diagnostic branch. The pre-branch
  source (`18681762^`) already used CAMetalDisplayLink with latency 1 and the
  same fixed preferred-rate request. The exact installed iPhone build was not
  supplied. This weighs against a universal portrait/120 Hz/Metal-link
  restriction; it does not isolate GPU family, panel, device settings or
  iPadOS window/presentation behavior. In particular, the failing iPad and
  working phone share the reported OS version and frame rate.
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

The second source pass adds a narrower experimental result: real Simulator
callbacks through the production link reuse the same IOSurface storage after
manual link recreation, while changing the drawable dimensions replaces that
storage. Thus the failed device recreation test did not test replacement of the
layer's storage or system presentation state. This does not prove stale storage
on the iPad or explain why backgrounding helped; see the
[experiment and limitations](evidence/metal-pool/README.md).

## Subsequent remote-test follow-up

The user subsequently authorized more Firebase tests. Build `202610301152`
(`20dd20d1`) replaced the SDL Metal view/layer at startup and orientation
changes; portrait remained Composited. That workaround has been removed.
The next diagnostic build runs the missing plain UIKit CAMetalDisplayLink
control once before SDL initialization. Its local Simulator check rendered
20 real Metal-link frames without SDL calls, handled resize and teardown, and
returned immediately on a subsequent unarmed startup. The physical-device
result for this control is still pending; the remaining-boundary discussion
above describes the evidence before this new build.

The user then supplied the missing control result: portrait Composited on
`527a57ec`, native UIKit window before SDL/bgfx initialization, CAMetalDisplayLink
with `update.drawable`, latency 1, 682 submitted frames. More Space was enabled;
the report shows 970×1408 points and 1940×2816 drawable/screen mode. Its visible
Metal layer is opaque, full-window, Linear filtered, BGRA8/sRGB, three drawables,
and untransformed/unclipped. This closes the earlier missing-control gap.
The one reported 8.360 ms callback interval does not establish presentation
timing. The phone control remains Direct at the same reported OS version/120 FPS.

The native probe shares the app bundle's `UIRequiresFullScreen = YES` setting.
The next candidate opts out of this compatibility mode on version 27 and later
using `UIRequiresFullScreenIgnoredStartingWithVersion = "27"`, while retaining
the existing Metal path. Apple documents different scene resizing behavior
for the compatibility flag in the 27 SDK. This is a remaining app-level policy
to isolate, not an established cause or proof of an OS bug. The automatic probe
is removed; normal startup resumes. See the
[candidate and test scope](../testing/ios-portrait-metal-diagnostics.md#current-candidate-native-scene-sizing-on-ipados-27).


The scene-policy build `202610301207` also remained Composited. The user wants
the policy retained and reported a screen-orientation regression. Its v5 report
exposes physical nativeBounds 1668×2420 / nativeScale 1.7196, while the current
mode and drawable remain 1940×2816. The next change replaces largest-mode sizing
with oriented nativeBounds for fullscreen windows, and adds SDL's modern scene
orientation-lock preference plus notification in the app's orientation function.
The real SDL/production-function Simulator smoke passes explicit modes and
gameplay lock/unlock; this runtime has no compatible iPad device. See the
[native-pixel and orientation follow-up](../testing/ios-portrait-metal-diagnostics.md#native-pixels-and-orientation-follow-up).
Neither iPad orientation recovery nor portrait Direct mode is yet confirmed.
