# iPhone / iPad presentation comparison

Status: unresolved compositor cause. Source comparison after Firebase build
`202610301218` (`ce4ec03a`), which the user reports still has nonfunctional
orientation selection and portrait Composited presentation. This pass makes no
new renderer changes. The user wants the native scene-policy opt-out retained.

## New device evidence

The v6 snapshot confirms that the setting reaches the native controller:

```text
commit=ce4ec03a43d0d72408548f328d13358303afa42d iOS=27.2 orientation=1
screen=970x1408 nativeBounds=1668x2420 nativeScale=1.7196 mode=1940x2816
BUNDLE requiresFullScreen=1 ignoreFullScreenFrom=27
SCENE coordinateBounds=970x1408 screenIsMain=1
ORIENTATION hint=LandscapeLeft LandscapeRight prefersLocked=1 effectiveLocked=1
APP_METAL_LINK attached=1 requestedRate=120 preferredFrameLatency=1
METAL drawable=1668x2420 pixelFormat=80 framebufferOnly=1
presentsWithTransaction=0 drawableCount=3 colorspace=kCGColorSpaceSRGB
```

The visible Metal layer is opaque, Linear filtered and untransformed. Its
contentsScale is 1.7192, computed from the exact native drawable dimensions and
logical bounds. The native-pixel correction has taken effect. The observed HUD
still says Composited, so the supersampled drawable mismatch is not sufficient
to explain the current result.

The orientation lock has also taken effect, but the scene remains portrait
while the requested mode is Landscape. This rules out a lost setting or a lock
preference that never reaches UIKit. It does not show why the rotation request
was denied or failed to take effect: the geometry error handler currently only
writes NSLog, and the snapshot does not retain its result.

## Scene policy is a real platform distinction

Apple's [WWDC26 UIKit session, interface orientation section](https://developer.apple.com/videos/play/wwdc2026/278/?time=410)
says supported-orientation preferences are ignored in a resizable environment.
The relevant distinction is the scene's environment, not simply the OS's
version number or the phone/pad idiom. An ordinary fullscreen iPhone app and a
resizable iPad scene can therefore treat the same preference differently.

The modern [orientation-lock preference](https://developer.apple.com/documentation/uikit/uiviewcontroller/prefersinterfaceorientationlocked)
holds the scene's current orientation. Apple makes honoring it conditional on
the scene being fullscreen, centered and unoccluded. It does not select the
requested axis. In this snapshot the effective lock is true, so failure to
satisfy the lock's eligibility conditions is not the explanation.

Our `screen_orientation::apply` sets the SDL orientation hint, invalidates the
lock preference, and only then requests geometry for the desired orientation
(`src/iOSNatives.mm:4520`). SDL returns a lock preference whenever the supported
mask contains only one axis (`SDL_uikitviewcontroller.m:224`), even when the
current scene has the other orientation. The result can therefore be a locked
portrait scene with a Landscape hint, exactly as observed. The earlier iPhone
Duo Simulator smoke allowed programmatic rotation with this lock enabled;
that is not evidence that the iPad accepts the same transition.

A correct transition needs to distinguish a requested orientation, the effective
orientation, and whether locking is appropriate. Locking the old orientation
before confirming the requested transition is not a sufficient implementation.
Unlock/request/observe/relock is an ordering hypothesis, not yet a demonstrated
iPad fix: a resizable environment may still refuse the geometry request.

The SDK also adds `UIWindowSceneDelegate.supportedInterfaceOrientations(for:)`
in version 27, replacing the app-delegate/default Info.plist source, and
`didUpdateEffectiveGeometry` in version 26. SDL still uses the old controller
mask and geometry notification. These are migration gaps, not evidence that
adding a scene mask will override the resizable-environment policy. The app
already declares all four orientations; no missing orientation in Info.plist
has been established. [Scene delegate API](https://developer.apple.com/documentation/uikit/uiwindowscenedelegate/supportedinterfaceorientations%28for%3A%29)

## What differs in the Metal comparison

Reported observations, collected on different builds/settings:

| Device / path | Portrait | Landscape |
| --- | --- | --- |
| iPhone 16 Pro, iOS 27.2, 120 FPS, earlier app with CAMetalDisplayLink | Direct | Direct |
| M5 iPad Pro, earlier native UIKit CADisplayLink triangle | Direct | Direct |
| M5 iPad Pro, native UIKit CAMetalDisplayLink triangle, More Space | Composited | Not established by the supplied native-control snapshot |
| M5 iPad Pro, current app, native-size drawable, CAMetalDisplayLink at requested 120 FPS | Composited | Current v6 report is portrait only |

The shared Metal-link code contains no phone/pad branch selecting a different
scheduler or presentation call: both use `update.drawable`, latency one and the
same fixed preferred-rate request (`src/video/MetalDisplayLink.mm`). The native
probe's only explicit idiom difference is support for upside-down orientation;
its Metal configuration and render/present sequence are shared. The failure
also occurs without SDL/bgfx initialization, so neither is necessary for that
native-control symptom. The exact installed iPhone build/settings are not known;
this is not a fully matched experiment isolating iPadOS from hardware.

The native CAD control acquires with `nextDrawable`; the native Metal-link
control receives an already acquired drawable through the callback. They share
the rendering and command-buffer presentation method. This leaves scheduling,
drawable delivery and system display handling as the useful comparison boundary.
Different iPhone/iPad display hardware remains a confounder. There is no evidence
here of a particular panel scan direction, GPU limitation, private compositor
flag, or a universal prohibition on portrait Direct presentation.

## What the current measurements cannot distinguish

The native control reports callback interval, not the interval between callback
arrival and `targetTimestamp`, command-buffer scheduling versus that deadline,
or `presentedTime` versus `targetPresentationTimestamp`. Apple's
[Metal-link contract](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink)
provides timing targets; these measurements would distinguish late submission
from composition despite timely submission. A steady 120 FPS callback rate does
not establish either outcome. Simulator timing is not a substitute for the
physical display path.

The [HUD metrics documentation](https://developer.apple.com/documentation/xcode/understanding-metal-performance-hud-metrics)
reports the selected composition mode and present delay, but does not expose
the compositor's rejection reason. Apple's frequently cited
[direct-display window guidance](https://developer.apple.com/documentation/metal/managing-your-game-window-for-metal-in-macos)
is explicitly for macOS; it is not an iPad scanout eligibility contract.

The current evidence establishes the wrongly oriented lock and successful
native sizing, but not a common root cause linking the orientation regression
and the older compositing symptom. Preserve that distinction rather than
attributing both to one unobserved iPadOS defect.
