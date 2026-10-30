# Native scene orientation transition regression

This optional local harness builds a small iOS Simulator app against this
checkout's rebuilt SDL framework. It extracts the production orientation helper
and `screen_orientation::apply` from `src/iOSNatives.mm`; it does not substitute
a duplicate implementation. It uses the actual SDL window/controller and real
UIKit scene geometry. No renderer or Firebase build is involved.

The initial test failed against `ce4ec03a` / SDL `d11765ae8` with:
`FAIL must not lock portrait while Landscape is pending`.

The corrected implementation passes these checks:

- Auto → Landscape → Portrait → Auto, then gameplay lock/unlock.
- The controller does not prefer locking the old, incompatible orientation.
- Every intercepted geometry request occurs after the effective lock releases.
- UIKit's effective-geometry callback reacquires the lock after rotation.
- An expired unlock deadline sends no geometry request.
- Auto cancels pending work; late errors from old requests cannot replace it.
- Synthetic geometry denial preserves code 101 for diagnostics and leaves the
  incompatible portrait orientation unlocked.

The denial check temporarily suppresses the phone's legacy mask-driven automatic
rotation and intercepts the public geometry-request method. Only that branch is
synthetic. All replacements are restored before exit. This checks error handling;
it does not prove why a physical iPad denies a particular request.

The available runtime is iOS 27.1, iPhone Duo only. Real mode transitions and
error-handling assertions pass there. The physical iPad orientation result and
Direct/Composited mode cannot be validated by this Simulator run.

Build with an existing **rebuilt** arm64 Simulator SDL3.framework:

```sh
python3 docs/reviews/evidence/ios-scene-orientation/build_simulator.py \
  --sdl-framework /path/to/Release-iphonesimulator/SDL3.framework \
  --output /tmp/asobmashow-orientation-evidence
xcrun simctl install booted /tmp/asobmashow-orientation-evidence/Smoke.app
xcrun simctl launch --console-pty booted com.snurhythm.sdl-orientation-smoke
xcrun simctl uninstall booted com.snurhythm.sdl-orientation-smoke
```

Successful output ends with `PASS actual SDL + production apply: unlocked
requests, confirmed relock, Auto/gameplay, timeout, cancellation, denial, stale
completion`. The test-only `Synthetic geometry denial` log is expected.
