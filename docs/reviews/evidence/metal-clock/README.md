# Metal refresh-clock source-audit harnesses

These are optional, standalone iOS Simulator research programs, not CI or app
features. They use real Metal callbacks and public rendering APIs. Runtime method
replacement is confined to these test processes; it is not shipped in the app.
Neither program measures physical iPad Direct/Composited eligibility.
The [recorded results](results.json) retain counts and transaction observations,
without private player data or raw absolute timestamps.

`sdl_clock_harness.mm` links the existing built SDL3 Simulator framework and
includes `IOSSDLRefreshClock.h`. Its screen-capability override exercises SDL's
automatic >60 Hz branch on a 60 Hz Simulator. This does not make the Simulator
run at 120 Hz. An observation hook counts SDL's otherwise no-op callbacks.
The app helper is invoked from a worker and dispatches to UIKit main. The test
requires SDL callbacks to stop while another 20 CAMetalDisplayLink frames render,
then resume after detachment, with a 120 Hz fallback request. `--60` verifies no
extra SDL clock is created on a display advertising 60 Hz. A no-op helper fails
the stopped-clock assertion. This fixture may emit SDL/UIKit appearance-transition
warnings while creating/destroying the standalone Metal view.

`transaction_harness.mm` includes the production Metal display-link and provider
implementation. A research-only delegate wrapper records actual callback
timestamps. The worker clears real supplied drawables and presents through a
command buffer. Main changes layer positions without animation, observing when
the presentation tree catches up. Default mode uses the application's nested
default/tracking run-loop pumping pattern; `--standard` uses UIKit's normal loop.
The harness stops after 120 frames with a 10-second bound. It applies no forced
transaction flush. Retry cooldown is timer-paced rather than busy-spinning.
The `displayed` column is unavailable/zero on Simulator, not zero latency.

Build from the repository root, using an already built arm64 Simulator SDL
framework from this checkout (the builder installs/uploads nothing):

```sh
python3 docs/reviews/evidence/metal-clock/build_simulator.py \
  --sdl-framework "$HOME/Library/Developer/Xcode/DerivedData/AsoBMaShow-FirebaseCI/Build/Products/Release-iphonesimulator/SDL3.framework" \
  --output /tmp/asobmashow-metal-clock-evidence
```

On an already booted Simulator, install and launch either artifact:

```sh
xcrun simctl install booted /tmp/asobmashow-metal-clock-evidence/ClockAudit.app
SIMCTL_CHILD_MTL_DEBUG_LAYER=1 xcrun simctl launch booted com.snurhythm.sdl-clock-audit
SIMCTL_CHILD_MTL_DEBUG_LAYER=1 xcrun simctl launch booted com.snurhythm.sdl-clock-audit --60
xcrun simctl install booted /tmp/asobmashow-metal-clock-evidence/Audit.app
SIMCTL_CHILD_MTL_DEBUG_LAYER=1 xcrun simctl launch booted com.snurhythm.metal-transaction-audit
```

The clock harness exits itself. Stop the transaction harness before relaunching
with `--standard`. JSON results go to each app's Documents directory, located
with `xcrun simctl get_app_container booted <bundle-id> data`. Uninstall these two
test bundle IDs after use. The real AsoBMaShow app/data are not used.
