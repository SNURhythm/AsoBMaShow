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

## Corrected orientation transition

The controller now requests a lock only when the effective scene orientation is
inside the requested mask. The app waits for an existing effective lock to clear
before issuing its geometry request (a bounded main-queue wait, at most one
second). SDL's effective-geometry callback re-evaluates the preference after
rotation. Auto or gameplay lock cancels obsolete queued work; late errors cannot
replace a newer request. Snapshot v7 retains request status and error domain/code
and reports effective orientation separately.

The [native regression harness](evidence/ios-scene-orientation/README.md) first
failed against the old controller, then passed with the corrected implementation,
including synthetic denial and cancellation. The unsigned iOS build passes.
The actual iPad outcome still requires device verification. No compositor change
or change to CAMetalDisplayLink pacing is included in this fix.

## Additional control: osu!lazer

The user reports osu!lazer is Direct on the same iPad in both orientations. Its
installed version/settings are not supplied. This is additional evidence against
a blanket inability of this iPad to present portrait content directly.

Current osu-framework commit `f05a31a9d82972e9b752060370ec17df17bdabda` uses
`SDL_SetiOSAnimationCallback` for its iOS loop, explicitly described as
CADisplayLink; the iOS host defaults to single-thread execution. Its project pins
`ppy.Veldrid` to `4.9.69-ga405fe8484`, whose Metal swapchain uses `nextDrawable`.
That backend presents via a command buffer followed by commit. Thus the checked
upstream path is not the CAMetalDisplayLink/update.drawable path used here.
This verifies source behavior, not the exact installed osu!lazer binary.

- [iOS loop](https://github.com/ppy/osu-framework/blob/f05a31a9d82972e9b752060370ec17df17bdabda/osu.Framework.iOS/IOSWindow.cs#L52-L63)
- [iOS execution default](https://github.com/ppy/osu-framework/blob/f05a31a9d82972e9b752060370ec17df17bdabda/osu.Framework.iOS/IOSGameHost.cs#L48-L54)
- [Pinned backend](https://github.com/ppy/osu-framework/blob/f05a31a9d82972e9b752060370ec17df17bdabda/osu.Framework/osu.Framework.csproj#L29)
- [Drawable acquisition](https://github.com/ppy/veldrid/blob/a405fe8484/src/Veldrid/MTL/MTLSwapchain.cs#L144-L150)
- [Presentation](https://github.com/ppy/veldrid/blob/a405fe8484/src/Veldrid/MTL/MTLGraphicsDevice.cs#L553-L557)

## Confirmed windowing-mode refusal

The subsequent v7 report from `151e5ee3` confirms the corrected transition
releases the lock (`prefersLocked=0`, `effectiveLocked=0`) before the request.
With Landscape requested (`mask=24`), the effective scene remains portrait
(`effective=1`, `matches=0`). UIKit returns `UISceneErrorDomain` code 101:

> The current windowing mode does not allow for programmatic changes to interface orientation.

The user then confirmed **Windowed Apps** is selected in Settings → Multitasking
& Gestures. This establishes a system-policy refusal for orientation, beyond
the earlier wrong-lock defect. Changing lock timing again will not override
that refusal. A window occupying the whole screen is not proof that the system
is in Full Screen Apps mode.

The relevant next check uses the existing build after choosing **Full Screen
Apps** and reopening the app. This is a device-wide setting; success in that
mode has not yet been observed. Keeping Windowed Apps while offering forced
portrait/landscape would require an application-owned content-orientation
implementation, including rendering and input coordinates. It is not achieved
by another call to requestGeometryUpdate.

Apple documents the three system modes and how to select them in
[Change Multitasking & Gestures settings](https://support.apple.com/guide/ipad/change-multitasking-gestures-settings-ipad-ipad2q9j404h2/ipados).
This setting recommendation targets the observed orientation denial. It does
not establish the cause of portrait Composited presentation: native 1668×2420
drawables, CAMetalDisplayLink at requested 120 FPS/latency one, and the reported
HUD state remain unchanged. The osu!lazer CADisplayLink control remains relevant.
