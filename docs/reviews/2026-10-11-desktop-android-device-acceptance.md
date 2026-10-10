# Desktop and physical Android acceptance

Verified on macOS with Metal and a Samsung Galaxy S20 FE (SM-G781N), Android 13,
with Vulkan. The user authorized an in-place Android update using version code 6.
The final installed APK is the normal signed restricted-file release build;
temporary diagnostic instrumentation was removed. No distribution upload ran.
Owned chart/archive/export fixtures were removed from the phone afterward;
the library returned to its original nine charts. Existing settings and data
were preserved. Desktop export artifacts were moved to temporary evidence storage.

## Results

| Check | Observed result |
| --- | --- |
| Desktop mouse and text | User confirmed Start, search entry and clearing worked. Native event logs matched the interaction. |
| Desktop export | 1280×720 H.264/AAC, 60 fps, 30 seconds, 1,800 frames. Full decode passed and a gameplay frame was inspected. |
| Desktop resize during export | Longer export completed with 5,400 frames while native window resizing/fullscreen operations were exercised. |
| Desktop quit during export | Native Cmd-Q cancelled export and completed bgfx shutdown; no API-thread assertion or Main Thread Checker violation. Debug bgfx reference-count warnings remained at shutdown. |
| Android stalled application owner | During a deliberately blocked 5-second owner interval, SDL main serviced 4,506 iterations and raw touch callbacks continued. |
| Android touch during that stall | Five injected DOWN events reached JNI ingress 1.891–4.126 ms after their Android event timestamps; native dispatch took 18–24 µs. Across all 15 callbacks: 1.891–6.969 ms and 5–24 µs. |
| Android completed export | 2400×1080 H.264/AAC, 120 fps, 30 seconds, 3,600 frames. Progress was visible; the app returned to records. Full decode passed and a gameplay frame was inspected. |
| Android export cancellation and surface recreation | Home at 17% cancelled export. Surface destruction began at 02:34:14.078, rendering suspension was acknowledged at 14.280, and SDL surface destruction continued at 14.281. Foreground returned to the visible records screen in the same PID; a subsequent export completed. |
| Android gameplay after export | Gameplay rendered normally, survived Home/foreground in the same process, and the pause menu remained responsive. |
| Android final-build frame pacing | SurfaceFlinger recorded 2,257 distinct presentation timestamps over 19.084 seconds of gameplay before Home: 118.21 fps, p95 8.548 ms, p99 8.658 ms, maximum 8.835 ms; no interval exceeded 12 ms. |
| Android MediaCodec surface probe | Passed colors, orientation, every-frame, thin-line and duration checks at 320×180/60, 2400×1080/60 and 2400×1080/120. |

The diagnostic stall was an application-thread sleep, not an injected GPU-driver
hang. A second 5-second stall while going Home reached the existing two-second
surface-retirement timeout. The process survived and resumed, but that is **not**
proof of safe GPU retirement under arbitrary stalls. Ordinary gameplay and the
final export cancellation did not reach that timeout.

These touch samples use ADB-injected Android timestamps; they do not measure
physical sensor-to-audio or sensor-to-photon latency. The pacing sample uses a
small synthetic chart and is not a heavy-chart performance benchmark. Windows
and Linux execution remain unverified because no hosts were available.

## Defects found and corrected

The first desktop export hit bgfx's API-thread assertion: its worker differed
from the application owner that initialized bgfx. Desktop and Android export now
run as queued application-owner work. Progress rendering stays on that owner
without reentering scene events. The independent SDL thread continues pumping.

Quit, actual application suspension and Android surface loss request export
cancellation. Desktop focus/minimize alone suppresses progress presentation but
does not cancel offscreen export. Stop requests also reach audio mixing, visual
loading and course parsing. Both records interfaces use the owner executor.

Actual phone testing then found a black screen after export cancellation:
combining surface availability with application lifecycle state could create a
background state without a matching foreground transition. Lifecycle activity
and presentation availability are now separate. A failing regression preceded
the fix, and the same-PID Home/foreground test above passed afterward.

The focused review found no remaining Important defects after its cancellation
and lifecycle findings were resolved.

## Separate storage failure

The synthetic gameplay score saved, but its replay did not:
`SavedWithoutReplay: Atomic replay install failed: Operation not permitted`.
The existing `ReplayFileStore` uses hard links for atomic installation; the
phone's emulated external Documents filesystem rejects that operation. A probe
in an owned scratch directory also found `renameat2(RENAME_NOREPLACE)` rejected
an unoccupied destination with `EINVAL` (an occupied destination gave `EEXIST`).

No persistence code was changed. A plain overwriting rename would weaken the
existing collision guarantees. Saved-replay round-trip acceptance remains
blocked by this separate filesystem compatibility issue; the exports above used
the built-in AUTO replay. Test score rows were retained rather than editing the
user's score database directly.

## Build and regression validation

- Desktop full build passed; full parallel CTest: **475/475 passed**, 199.41 s.
- After the final surface-state correction, the updated build and the three
  export/runtime regression tests passed again (0.96 s).
- Android restricted release build passed and was installed with `adb install -r`
  using version code 6; existing app data was preserved.
- Unsigned iOS build and artifact audit passed. The final surface-state change
  is in the non-iOS runtime; shared export cancellation changes were compiled.

Filtered logs, export metadata, inspected frames, final device screenshots and
raw pacing samples are retained in
[the evidence directory](evidence/2026-10-11-desktop-android-device/).
