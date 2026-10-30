# Preserve explicit drawable pixels across unchanged SDL layouts

This optional native regression harness links the real SDL3 Simulator framework
and extracts the current `SetIOSMetalLayerDrawableSize` implementation from
`src/iOSNatives.mm`. It runs on UIKit main, creates an actual SDL Metal view,
and applies the recorded iPad geometry (970×1408 points, 1668×2420 native pixels).
It does not simulate iPad compositor policy or claim physical-device timing.

```sh
python3 docs/reviews/evidence/metal-sizing/build_simulator.py \
  --sdl-framework /path/to/Release-iphonesimulator/SDL3.framework \
  --output /tmp/asobmashow-metal-sizing-evidence
```

Install the resulting `Sizing.app` on an available Simulator and launch
`com.snurhythm.metal-sizing-audit` with `simctl launch --console`. Require the
`PASS actual SDL + production sizing` message; simctl's exit code alone does
not establish success after an assertion abort. Terminate/uninstall afterward.

Before the fix, the native run printed:

```text
before layout size={1668, 2420} scale=1.719168814433
after layout size={1667, 2420} scale=1.719168814433
FAIL unchanged foreground layout must retain exact native drawable size
```

The app's averaged scale produces fractional dimensions; CAMetalLayer
canonicalizes them to integer pixels. After the fix, both before/after values
remain `{1668, 2420}`, and scale remains `1.719587628866`.
The harness additionally checks that repeated layouts emit no Metal resize,
while a genuine bounds change and a genuine scale change each resize and emit
an event. An explicitly selected render size survives a later unchanged layout.

One initial follow-up assertion incorrectly expected the native layer to retain
fractional dimensions after a real resize. It was corrected to the adjacent
integer pixel bounds; the original exact-native-size regression was unchanged.
Early native view teardown can print UIKit appearance-transition warnings after
the PASS message; this harness is not a scene-animation test.
