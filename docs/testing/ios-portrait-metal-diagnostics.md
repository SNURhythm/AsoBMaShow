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
