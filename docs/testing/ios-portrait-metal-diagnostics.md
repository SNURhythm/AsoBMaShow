# Portrait Metal presentation experiment

The follow-up source fix uses Linear at normal iOS startup, handles Metal-view
resize notifications, and rejects drawables that do not match the renderer's
attachments while keeping fallback rendering compatible. Native regressions and
the unsigned iOS build pass. See the
[source audit and fix](../reviews/2026-10-30-ios-metal-source-audit.md).
The observations and manual instructions below describe the earlier experiments;
in particular, older statements about a cold launch restoring Nearest are historical.
The portrait Direct/Composited cause remains unconfirmed.

The user additionally reports that an iPhone 16 Pro on iOS 27.2 at 120 FPS
was Direct in both orientations on a build predating this branch. The original
source already used CAMetalDisplayLink. This makes the failing iPad's device
and presentation configuration a key distinction; 120 FPS or the reported OS
version alone cannot explain the difference. It does not establish an iPadOS
bug; the plain UIKit/Metal-link result is recorded below.

## Current candidate: native scene sizing on iPadOS 27

The plain UIKit control also remained Composited in portrait on build
`202610301157` (`527a57ec`). Its report confirms `SDL not initialized`,
`bgfx not initialized`, CAMetalDisplayLink/update.drawable, latency 1, 682
submitted frames, and one 8.360 ms callback interval. The user confirmed More
Space was enabled: window 970×1408 points, drawable/screen mode 1940×2816,
opaque BGRA8/sRGB, three drawables, Linear filters and no transforms or clipping.
That callback interval is not a presentation-latency measurement. SDL and bgfx
are not necessary to reproduce this case, but the native probe still shares
the application bundle's scene configuration.

The next candidate adds `UIRequiresFullScreenIgnoredStartingWithVersion = "27"`.
It retains `UIRequiresFullScreen = YES` for older systems, but opts out of that
compatibility behavior on version 27 and later. Apple documents discrete scene
resizing for apps linked against the 27 SDK that retain this compatibility
flag. This is an unverified scene-policy hypothesis, not proof of the portrait
cause. The application already supplies a launch screen, all orientations,
SDL scene lifecycle and resize handling. Window resizing can now follow the
normal iPadOS policy. CAMetalDisplayLink, latency, rate and drawable count stay
unchanged. See [Apple TN3192](https://developer.apple.com/documentation/technotes/tn3192-migrating-your-app-from-the-deprecated-uirequiresfullscreen-key?changes=_3_3).

Normal application startup is restored. The native triangle remains available
only when explicitly armed in Settings. Snapshot v5 reports the installed
bundle's full-screen keys and scene bounds. Check the normal app in fullscreen
portrait and landscape, keeping More Space unchanged for this comparison.

## Native pixels and orientation follow-up

Build `202610301207` (`e3e77e9d`) remained Composited. The user explicitly
requested retaining the native scene policy, and reported that the screen
orientation control no longer worked. Snapshot v5 confirms the opt-out is active
and `screenIsMain=1`. With More Space still enabled, nativeBounds now reports
**1668×2420**, nativeScale **1.7196**, while currentMode and the Metal drawable
remain **1940×2816** at scale 2. The older sizing helper deliberately picked the
largest candidate, overriding the physical panel dimensions.

The follow-up uses oriented nativeBounds for an actual fullscreen window,
including downsizing and correcting one-pixel nativeScale rounding. Logical UI
points are preserved; partial windows and stale resize events retain SDL sizing.
Focused tests cover the recorded portrait/landscape geometry, normal phone
geometry, fractional rounding, partial windows, invalid aspect and stale events.
This corrects the reported sizing discrepancy; it does not explain every earlier
portrait failure or guarantee Direct mode.

For orientation, the SDL controller now supplies iOS 26's
[`prefersInterfaceOrientationLocked`](https://developer.apple.com/documentation/uikit/uiviewcontroller/prefersinterfaceorientationlocked)
when its hint restricts orientation to one axis (or a single gameplay orientation).
The application invalidates this preference before its existing geometry request.
Auto restores both axes and clears the preference. Apple only honors the scene
lock when fullscreen, centered and unoccluded; windowed iPad orientation remains
under system control. Snapshot v6 records the orientation hint, requested lock
and effective scene lock to distinguish the two.

A local smoke app using the rebuilt SDL framework and the production
`screen_orientation::apply` function passed Auto → Landscape → Portrait → Auto
and gameplay lock/unlock, asserting the effective scene state. The installed
27.1 Simulator runtime supports only iPhone Duo, so this is API/integration
coverage, not verification of the reported iPad behavior. The physical iPad
orientation and Direct/Composited result remain pending.

The subsequent physical-device v6 result confirms exact 1668×2420 drawables,
but the HUD remains Composited. With Landscape selected, the report shows
`orientation=1`, `prefersLocked=1`, and `effectiveLocked=1`: the system accepted
the lock while the scene remained portrait. The previous Simulator pass did
not validate this iPad transition. See the
[iPhone/iPad comparison](../reviews/2026-10-30-ios-ipados-presentation-differences.md)
for the source-policy distinction and remaining measurement limits.

## Completed control: UIKit with CAMetalDisplayLink

The user reported that build `202610301152` (`20dd20d1`) still shows Composited
in portrait. Surface replacement did not resolve the symptom and has been
removed, including its extra startup/rotation work.

Build `202610301157` automatically ran the missing plain UIKit
CAMetalDisplayLink control once, before SDL initialization. It uses the same
native triangle renderer as the earlier CADisplayLink control, now with
`update.drawable`, requested latency 1 and the display's maximum refresh rate.
No SDL window, bgfx renderer or SDL refresh clock exists in this control.
The one-time request is consumed before starting, so force-quitting cannot trap
subsequent launches. Double-tap to copy the report and continue normal startup.
Settings → Display → Native Metal display link on next launch can repeat it.

Read the Metal HUD in portrait and landscape on the triangle screen. This is
a missing comparison, not a claimed fix. A Direct result would make SDL/app
integration worth isolating further; a Composited result would reproduce it
without those components, but would not by itself prove an OS defect.

## Failed candidate: replace the SDL Metal surface

The user subsequently requested another Firebase candidate for remote device
testing. This candidate replaces the SDL Metal view and its CAMetalLayer once
after startup initialization and on portrait/landscape changes. It detaches
the old display link, rebinds bgfx and gameplay touch input, consumes the native
handle change with a discarded frame, and releases the old view on main before
resuming CAMetalDisplayLink. Input stays paused if it was paused before rebinding.
Frame-rate and resolution policy, requested latency 1 and three drawables remain
unchanged. Ordinary frames do not replace the surface.

This tests layer-owned presentation state that a display-link-only restart
retained. It is an unverified workaround, not an identified portrait root cause.
The native Metal regression confirms that handoff does not acquire from either
layer during discard and that the next rendered frame uses only the new layer.
A local SDL Simulator smoke check completed four replacements and 32 real Metal
callbacks, preserving the controller and rebinding the production recognizer,
including its disabled state. The rapid replacement harness logged UIKit
appearance-transition warnings; it did not crash or lose the replacement view.
Simulator cannot validate the physical iPad's Direct/Composited behavior.

The tester reported portrait still Composited; the candidate is retired.

## Earlier candidates

Device follow-up on Firebase build `202610301045` (commit `2c868e6f`) confirms
that portrait still shows Composited. The resize and filter corrections did not
resolve that symptom; their passing native tests do not establish Direct mode.

A later source audit found SDL's implicit high-refresh CADisplayLink running
beside the app's CAMetalDisplayLink. Normal rendering now suppresses that SDL
clock while the Metal link is attached, restoring SDL's refresh request for
timer fallback. CAMetalDisplayLink remains the rendering scheduler. The older
SDL-surface probe results below included the implicit SDL clock and must not
be treated as a test of the Metal link alone. Following distribution of build
`202610301116` (`552f60b2`), the user again reports portrait Composited. This
change did not resolve the reported symptom; see finding 3 in the source audit.

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

## CAMetalDisplayLink device results

Build `d90141f1062d6b690ee5e6a2ee6c136b312c11d7` reproduced Composited in
portrait with the SDL CAMetalDisplayLink probe, before bgfx initialization.
The report identifies `update.drawable` and preferred frame latency one.

With Display Zoom enabled, the tester then reported Composited for normal
rendering and Direct for the same probe in landscape (orientation 3). Both
snapshots report 1408×970 window/layer bounds, scale/nativeScale 2, 2816×1940
drawables, opaque BGRA8/sRGB, three drawables, identity transforms and the same
secondary window. All captured layer properties match. This is not evidence
that CAMetalDisplayLink universally prevents Direct mode.

## Filter and foreground-return results

On build `b15bd311545ede102cda2d7ef1e3e9920bae5d24`, normal rendering in
landscape with Display Zoom enabled changed Composited → Direct → Composited
when the tester selected Nearest → Linear → Nearest. The three v3 snapshots
differ only in the CAMetalLayer's magnification filter. This isolates the filter
as a cause in that configuration; no FPS or latency improvement was measured.

In portrait, the tester reports initial composition with Linear, followed by
Direct after spending some time backgrounded and reopening, with Zoom enabled
and disabled. They subsequently observed the same symptom with Nearest in
portrait. The portrait recovery therefore is not specific to Linear. Backgrounding
invalidates the app's CAMetalDisplayLink, and foreground rendering creates another;
elapsed time, other app lifecycle work and system presentation state also change.
These observations do not identify which of those causes the transition.

## Historical test: foreground waiting versus display-link recreation

The user reports that manual link recreation did not help and stopped manual
testing on 2026-10-30. These instructions are retained as experiment provenance,
not a request to repeat the test. The subsequent
[source audit](../reviews/2026-10-30-ios-metal-source-audit.md) records a locally
reproduced resize/MSAA defect and the remaining limits of the portrait diagnosis.

Keep orientation, Zoom, magnification filter and frame cap fixed within each
trial. Use normal app rendering in portrait and reproduce Composited before
starting each trial. A cold launch can help reproduce the state, but also resets
the filter to Nearest: explicitly reselect Linear if that is the chosen test.

1. **Foreground wait:** copy the baseline diagnostics, leave the app visible for
   the same duration previously spent backgrounded, and copy again. Record HUD
   mode, Present Delay and elapsed time. Do not change settings or recreate the
   link during this trial.
2. **Recreate only:** reproduce the same starting state, copy diagnostics, then
   choose **Recreate Metal display link (test)** in Settings → Display. Stay in
   the foreground, record whether the HUD changes immediately or after waiting
   the same interval, and copy diagnostics again.
3. **Background control:** reproduce the starting state once more, copy, then
   background for that same interval, reopen and copy. Record the HUD outcome.

Snapshot v4 adds `APP_METAL_LINK` evidence captured on the application owner
thread before the UIKit layer snapshot. `creationSerial` counts successful link
creations; `linkAgeSeconds` is time since that creation. `capturedAtMediaTime`
allows elapsed-time comparison. `manualRequests`, `manualRecreations` and
`recreationPending` distinguish a queued request from an actual replacement.
For the recreate-only trial, require `manualRecreations` to increase by one,
`recreationPending=0` and `attached=1`. An automatic creation during the wait
trial also increases `creationSerial`, so that trial was not purely a wait on
the same attachment. The cold-launch probes report no app-link owner on their
calling thread; their own scheduler is still identified by their probe header.

The manual action runs only on the application/Metal owner thread. It queues
replacement until the next beginFrame, after retiring the current lease. Repeated
requests before that boundary coalesce; a lifecycle/export pause cancels the
pending request. It adds no GPU drain, deliberate fallback frame, artificial delay, saved
setting or automatic restart policy. It does not change the filter, layer size,
drawable pool, requested frame rate or preferred frame latency one. Reports
confirm link creation, not actual presentation or the compositor's decision.

Focused native coverage checks request deferral/coalescing, one-time replacement,
preserved layer/pacing state, report counters and cancellation. It uses a real
Metal link with a deterministic run-loop wake, not physical display callbacks.
The existing frame-pacer tests and unsigned iOS build also pass. Device HUD and
latency remain unverified.

## Layer magnification filter instructions

The bgfx Metal backend sets `magnificationFilter` to `nearest` during swap-chain
initialization. The native probes leave it untouched; Apple's documented default
is `linear`. Snapshot v2 omitted this property. Snapshot v3 records magnification,
minification and minification bias for every layer. The filter difference is a
candidate to isolate, not a confirmed compositor cause.

1. With Display Zoom enabled, open normal rendering in landscape, then
   Settings → Display. Under **Metal magnification filter (test)**, check that
   **Nearest** is selected. Record the HUD mode and Present Delay, and copy the
   diagnostics before changing the filter.
2. Choose **Linear**, let the HUD settle, and repeat the capture. Check the
   CAMetalLayer's `magnificationFilter=linear` in the new report.
3. Choose **Nearest** again and repeat to check whether any change reverses.
4. If the HUD mode changes, repeat the same test in portrait and with Display
   Zoom disabled. Keep orientation and other settings fixed within each test.

The control reads the actual active Metal layer and changes only its
magnification filter, on the UIKit main thread with implicit animations disabled.
It does not change minification, resolution, frame cap, scheduler, drawable count,
or preferred frame latency. There is no saved setting or per-frame override;
Nearest restores the existing app behavior, and a normal cold launch resets it
through bgfx initialization. Layer scaling appearance may differ, so this remains
a reversible experiment rather than a default rendering change.

Local verification: the final unsigned iOS build and `git diff --check` passed.
The control and native bridge were reviewed for current-value selection,
main-thread mutation and bounded lookup. HUD mode, appearance and performance
effects require the physical-device comparison above.

## CAMetalDisplayLink probe instructions

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

## Orientation transition correction (snapshot v7)

Explicit mode changes release an incompatible orientation lock and wait for
UIKit's effective unlock before requesting rotation. The controller requests
locking again only after the effective scene orientation matches the setting.
Auto and gameplay lock invalidate obsolete pending requests. The one-second
unlock deadline bounds the asynchronous wait; no retry loop runs during normal
rendering. Snapshot v7 adds `ORIENTATION_REQUEST` with the effective orientation,
mask match, request status and any UIKit error. A denied geometry request leaves
the incompatible scene unlocked instead of locking the wrong orientation.

Native Simulator integration tests cover actual rotation/relock, Auto/gameplay,
unlock timeout, cancellation and a synthetic refusal. This repairs the verified
lock-order defect, but does not establish that iPadOS will accept the geometry
request or change the Metal HUD mode. CAMetalDisplayLink remains enabled.
