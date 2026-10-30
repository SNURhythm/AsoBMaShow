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

## Home indicator A/B test (snapshot v2)

The next build adds **Hide home indicator: Off/On** beside the copy button.
This is a temporary, session-only experiment, not a persisted setting or a fix.
Startup behavior is unchanged. On sets `SDL_IOS_HIDE_HOME_INDICATOR=1`; Off resets
that hint to SDL's default. Restarting the app also returns to the default.

1. In portrait, leave the test Off and confirm the baseline HUD mode.
2. Turn it On. Stop touching the screen, wait for the home indicator to disappear,
   then record the settled HUD mode and Present Delay. Merely setting the request
   does not prove that iPadOS actually hid the indicator.
3. Copy the diagnostics and report whether the indicator visibly disappeared.
   The copy tap can reveal it again; report the HUD state from before that tap.
4. Turn the test Off and repeat the observation. Check landscape with On as well.

Report Off → On → Off results, plus the v2 report's `homeIndicatorHint` and
`prefersHomeIndicatorHidden` values. Another full report is only needed if the
mode or geometry changes.

SDL couples this hint to system gesture deferral: default fullscreen behavior
dims the bar and defers edge gestures; auto-hide requests no edge deferral.
Consequently a home swipe may act immediately while On. The report includes
`deferredGestureEdges` to make that difference explicit. A positive result
implicates this combined system-UI request, not necessarily the bar alone.
An unchanged result does not establish a hardware limitation.

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
