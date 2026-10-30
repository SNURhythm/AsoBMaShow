# Portrait Metal presentation experiment

This diagnostic branch investigates a report from an 11-inch M5 iPad Pro on
iPadOS 27.2: the Metal HUD stays Composited in portrait and returns to Direct in
landscape. Reported drawable sizes are native (1668×2420 and 2420×1668), with
Display Zoom disabled. The cause is not established; this is not a fix.

The first Firebase test (build commit
`b5dc0d4ec360a8e515a6f6051189a33940fe48f1`) confirmed Composited in both portrait
orientations (1 and 2), and Direct in landscape orientation 3. All reported
Metal properties matched apart from width/height. The layer filled the window,
was opaque, and had identity transforms, no mask, and no animations. The same
level-10 secondary window existed in both modes. This provides no obvious
app-layer explanation, but does not exclude system overlays or compositor policy.

## Native Metal cold-launch test

Device result (`d73728b52f41290c700d2abdba87a55168224ae6`): the native test was
Direct in both orientations. Portrait remained native 1668×2420 with a reported
8.333 ms callback interval. This disproves an unavoidable portrait restriction
on this device. The level-10 window also remained present, so its mere presence
does not explain the original app's behavior. The triangle's changing proportions
were a probe shader issue (uncorrected normalized coordinates), now corrected
using the drawable aspect ratio; the surface itself was not incorrectly sized.

## SDL-surface baseline result

Device result (`1a38b3f94b78c200a6e0a6f8de09a9735928f617`): the SDL-surface
native test was Direct in both orientations. The portrait report identifies
`SDL-created surface; bgfx not initialized`, with 486 submitted frames and an
8.333 ms display-link timestamp interval (not presentation latency). Its drawable
remained native 1668×2420, opaque BGRA8/sRGB, with three drawables and identity
transforms. The level-10 secondary window and `prefersHomeIndicatorHidden=0`
were present while Direct. Neither that window's presence nor that preference
alone explains the normal app's portrait composition.

This establishes that the SDL-created surface can present Direct in portrait.
It does not establish that bgfx is the cause: the native probe also differs in
drawable acquisition, scheduling, execution thread, and rendering workload.

## Next test: CAMetalDisplayLink on the SDL surface

Choose **SDL Metal display link on next launch**, close the app from the app
switcher, and reopen. Let rotation settle and record the triangle screen's HUD
mode and Present Delay in both orientations. Double-tap in portrait to copy the
report and continue startup. Check for `scheduler=CAMetalDisplayLink`,
`drawableSource=update.drawable`, and `preferredFrameLatency=1` in the report.

This keeps the SDL surface, main-thread execution, native Metal triangle,
pipeline, command submission/presentation, and no-bgfx startup boundary from the
successful SDL probe. It replaces CADisplayLink/nextDrawable with
CAMetalDisplayLink and its supplied drawable, requesting one frame of rendering
latency as the normal app does. It does not reproduce the app worker or its
frame-handoff logic. The new mode reports actual callback arrival intervals
using CACurrentMediaTime; the older CADisplayLink modes report link timestamp
intervals. Neither measurement is presentation latency.

The request remains one-shot. The link is invalidated and its delegate released
before the last command drains and normal bgfx startup acquires the layer.
iOS versions below 17 report an unsupported-test error and resume startup rather
than silently running a different scheduler.

Composited in portrait would show that the symptom can occur without bgfx or
gameplay when this display-link/drawable path is used. Direct would leave bgfx,
the app's worker/handoff, workload, and other later startup differences to isolate.
Neither outcome alone establishes the compositor's internal reason.

Apple's [delegate documentation](https://developer.apple.com/documentation/quartzcore/cametaldisplaylinkdelegate/metaldisplaylink(_:needsupdate:))
describes rendering into the supplied drawable; the probe retains ordinary
command-buffer `presentDrawable` with no explicit presentation-time variant.

Local verification for the new mode: the unsigned iOS build passed. An isolated
Simulator harness compiled the actual probe source and exercised all three
modes, each submitting at least ten frames, including after drawable resizing.
It checked scheduler identity, the one-frame latency request, consumed/deferred/
unarmed startup, snapshot labeling, exit gesture removal, release of both links
and the delegate, and ordinary nextDrawable acquisition after teardown. SDL event
pumping, raw-touch draining, main-thread dispatch and snapshot capture were
stubbed; the external test surface was a UIKit Metal view. This does not verify
real SDL rotation, full app re-entry, or the physical iPad's compositor decision.

## SDL-surface baseline instructions

Choose **SDL surface on next launch**, close the app from the app switcher,
and reopen. Record the triangle screen's HUD in both orientations, then
double-tap in portrait to copy the report and continue startup. The report starts
with `Native Metal probe: SDL-created surface; bgfx not initialized.`

This uses the same native triangle renderer and CADisplayLink as the successful
native-window test, but runs on the actual SDL-created CAMetalLayer. It keeps
SDL's window, root controller, view hierarchy and rotation/layout handling.
It runs before gameplay touch installation and bgfx initialization. SDL pumps
events; probe pointer/key input is discarded while lifecycle/file-open events
are retained. On exit, the native queue drains once before bgfx can use the layer,
the temporary exit gesture is removed, and startup re-reads the window dimensions.

Direct in portrait would move the investigation beyond SDL window setup toward
the later rendering/workload/presentation path. Composited would implicate
differences in SDL window/view setup or surrounding UI, not establish a specific
cause. The host controller's system-UI preferences remain SDL's own, unlike the
native probe controller. Neither test changes the app's normal rendering path.

## Native-window baseline instructions

1. Install the latest Firebase build and open Settings → Display.
2. Tap **Native Metal on next launch**. After the armed message appears, close
   the app from the app switcher and reopen it (backgrounding alone is not enough).
3. A green triangle moving across a dark blue background is the native test.
   There are no visible app controls. Let rotation settle and record the HUD mode,
   resolution, refresh rate, and Present Delay in landscape and portrait.
4. While in portrait, double-tap the surface. This copies a native-test snapshot
   to the clipboard and continues normal app startup. Paste it with your observed
   HUD modes. The snapshot identifies `AsoNativeProbeMetalLayer`.

The request is consumed before running, so force-quitting during the test makes
the next launch normal. To repeat, arm it again in Settings. If setup fails,
normal startup continues and the clipboard contains the failure reason.

The test runs at the start of `AsoBMaShowMain`, before `SDL_Init`, SDL window
creation, bgfx initialization, and the app/audio worker. It creates a native
UIKit window with a `CAMetalLayer`-backed view, a Metal device/queue, and a minimal
triangle pipeline. A `CADisplayLink` requests the screen's maximum refresh rate;
each callback draws and presents directly with Metal. Background/inactive scenes
submit no frames. Double-tap invalidates the display link before removing the
window. The probe has native-scale BGRA8, an opaque sRGB layer, three drawables,
and no additional native UI overlay. Startup/launch windows are hidden during it.

This isolates SDL's window/view setup and bgfx/app rendering, but retains the
same app identity, Info.plist, SDL UIKit bootstrap/scene delegate, system UI,
and device/OS. It also uses a different workload and frame scheduler. Direct in
both orientations would narrow the issue to differences in the normal app path;
Composited in portrait would show the symptom survives this simpler native path,
not prove that every iPad app or all hardware must behave that way. The reported
callback interval is not GPU completion time or presentation latency.

Local verification: the unsigned iOS app builds successfully. An isolated
Simulator harness compiling the actual probe source submitted ten frames,
checked native layer properties and drawable resizing, invoked the exit handler,
verified display-link invalidation, and verified consumed/unarmed startup.
The harness stubbed the snapshot-copy boundary and main-thread dispatch; it did
not exercise the full app's subsequent SDL startup or the physical iPad's
presentation mode. A separate manual harness run visually confirmed the triangle
on the Simulator's active display. The temporary harness app was then removed.

## Home indicator experiment result

The tester reports that home-indicator hiding already existed and the added
Off/On experiment made no difference. The toggle has been removed; do not repeat
this experiment or treat it as a fix. Snapshot v2 retains the read-only hint and
gesture-deferral fields. The HUD label alone does not identify the underlying
compositor decision or measure its performance cost.

## Documentation check

The captured state already satisfies the proposed opacity, pixel-format, sizing,
and layer-transform checks: `opaque=1`, `opacity=1`, pixel format 80
(`MTLPixelFormatBGRA8Unorm`), scale 2, native dimensions, and identity transforms.
BGRA8 includes an alpha channel; switching to it cannot be a missing fix because
it is already in use. The reports do not show a manually rotated Metal sublayer.

Apple's [window setup article](https://developer.apple.com/documentation/metal/managing-your-game-window-for-metal-in-macos)
is specifically about macOS/Apple silicon and explicitly allows hardware/software
edge cases. It describes direct-display scaling, so exact native resolution is
not a universal prerequisite for that path. It does not guarantee Direct mode
on an iPad or offer an API to force it.

The [Metal 4 UI modes](https://developer.apple.com/videos/play/wwdc2025/211/)
are options for supplying UI to MetalFX frame interpolation, not controls for the
window compositor's Direct/Composited decision. This app does not use MetalFX
frame interpolation.

Apple's [HUD explanation](https://developer.apple.com/videos/play/tech-talks/110339/)
says compositing may add buffering/latency. It does not establish that this
particular portrait case has degraded FPS or frame pacing, or that it must be an
app optimization defect. A minimal native Metal reproduction on the same iPad
would distinguish app integration from a device/OS behavior more effectively
than reapplying layer settings already confirmed by these snapshots.

## Baseline Firebase device test

The PR targets `develop`, triggering the existing iOS Firebase workflow.

1. Install the Firebase build and open Settings → Display in landscape.
2. Allow rotation to finish and check that the HUD still says Direct on this
   settings screen. Tap **Copy Metal diagnostics**, then paste the report into
   your reply or a note. Include the observed HUD mode and Present Delay.
3. Return to the same screen in portrait, allow rotation to finish, and repeat.
   Paste each report before copying another: the clipboard holds one snapshot.
4. Also report whether a fresh app launch while already in portrait starts
   Composited. If the HUD behavior differs between settings and gameplay, say so;
   a settings snapshot does not describe an earlier gameplay frame.

The snapshot records build identity, OS version, orientation, screen geometry,
app-window state, layer geometry/transforms/clipping/opacity, and Metal drawable
configuration. It captures on the UIKit main thread only when requested, before
writing to the clipboard. It does not force layout, acquire a drawable, add a
native overlay, collect continuously, or change presentation settings.

Traversal is capped at eight app windows, 128 layers, and depth 16. No layer
contents, text, accessibility labels, user-assigned names, or object addresses
are collected. There is no automatic upload or diagnostic file persistence.
System compositor overlays and the actual Direct/Composited decision are not
available through this app-layer snapshot; record the HUD mode separately.

Compare the two reports before trying a presentation change. Matching drawable
sizes do not prove identical layer geometry or direct-display eligibility. If
app-layer state does not explain the difference, a physical-device system trace
or a separate minimal Metal reproduction will still be needed. Remove this
temporary English-only diagnostic control after the investigation.
