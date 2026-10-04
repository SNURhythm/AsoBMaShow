# Android graphics corruption investigation

The reported 2340 × 1080 screenshot has unreadable menu content and regular
horizontal black bands across both text and solid-color panels. The responsible
phone, GPU, driver, and active renderer have not been captured. Source review has
not established the cause.

Physical Android devices previously requested a 2× MSAA backbuffer. Our working
emulator configuration used GLES without MSAA, so it did not validate that path.
Android now defaults to no MSAA as a provisional compatibility mitigation. Vulkan
remains the first automatic candidate, followed by GLES if initialization fails.
The API 33+ emulator exception still selects GLES automatically.

bgfx's internal fallback is disabled for each Android attempt. Previously it
could switch renderer inside `bgfx::init`, bypassing the application's next
candidate and its backend-specific color format. The application now owns that
fallback and verifies the actual renderer before starting the UI. This is a
confirmed initialization issue; it has not been linked to the reported image.

## Controlled comparisons

Use a freshly built APK. Set `adb` to the Android SDK's platform-tools executable
if it is not on PATH. Each comparison must start a new process; extras delivered
to an already running activity do not reinitialize the renderer.

```sh
adb shell am force-stop com.snurhythm.asobmashow
adb logcat -c
adb shell am start --user 0 -n com.snurhythm.asobmashow/.AsoBMaShowActivity --es graphics_backend vulkan --es graphics_msaa 0
```

Repeat with these combinations, force-stopping before each launch:

| `graphics_backend` | `graphics_msaa` | Comparison |
| --- | --- | --- |
| `vulkan` | `0` | Vulkan without the MSAA resolve path |
| `vulkan` | `2` | Previous physical-device MSAA request |
| `gles` | `0` | GLES at the same sample setting as the first run |
| `gles` | `2` | Whether corruption follows MSAA across both backends |

`graphics_backend` accepts `auto`, `vulkan`, or `gles`; `graphics_msaa` accepts
`0` or `2`. Both extras are strings (`--es`). Invalid recognized values abort
startup with a log message. Forced backends do not fall back: a failed Vulkan
initialization must not be mistaken for a successful Vulkan rendering test.
Omitting the extras restores defaults on the next fresh launch; nothing is saved.

The SDL window still uses its existing Vulkan-capable window setup, including
loading the system Vulkan library. A forced GLES test isolates rendering after
window creation; it does not bypass a failure to create that window.

```sh
adb logcat -d -v time | rg 'Android device:|Android graphics:|bgfx renderer:|bgfx reset flags:|Invalid Android graphics option'
adb exec-out screencap -p > android-graphics.png
```

The logs record manufacturer, model, hardware/SoC identifiers when available,
Android SDK/build, selected renderer, bgfx GPU IDs, drawable dimensions, requested
color format and MSAA, and reset flags. `requestedMSAA` is the application request;
the driver/backend may choose a supported sample count. GPU IDs can be incomplete
on mobile, so retain the device identifiers too.

A clean no-MSAA Vulkan image and broken 2× image on the same phone would implicate
the multisample path. Broken Vulkan at both settings with clean GLES would instead
narrow the issue to the Vulkan path beyond MSAA. Emulator success alone cannot
confirm resolution of the reported physical-device corruption.

## Local verification (2026-10-04)

The signed Firebase-flavor release APK built successfully without upload, and
the desktop application and graphics-options regression test passed. The full
CTest run passed 420 of 421 tests; the coroutine callback test exceeded its 4 ms
wall-time guard during concurrent compilation, then passed in isolation after
compilation stopped. No timing limits were changed.

An API 29 `android10` emulator with software graphics was exercised at
2340 × 1080. Unlike the earlier API 33 emulator, it initialized Vulkan
(vendor `0x1ae0`, device `0xc0de`). Automatic and forced Vulkan without MSAA, and forced GLES
with both sample requests produced readable intro screens. Forced Vulkan with
2× MSAA initialized but remained black for over a minute. The GLES no-MSAA run
also lost visible content after attempting to enter the menu, so scene-transition
validation is incomplete. These observations are not a reproduction of the
reported horizontal bands and do not establish a physical-device fix.

## Shader audit and follow-up fixes

The packaged Android shaders matched the repository. All 19 committed Vulkan
modules passed `spirv-val`; fresh compilation matched all 19 GLES shaders.
Vulkan instruction differences were limited to equivalent repeated calculations
in three fragment shaders, with no nonfinite constants found.

The audit found and fixed three separate issues: the legacy Makefile compiled
the first shader of each stage into unrelated output names; incremental builds
ignored shared includes and varying definitions; and distance-field text used
undefined `smoothstep` edges when shadow smoothing was zero. Make now delegates
to the Python compiler path, which also tracks recursive includes, the compiler,
and build-script changes. Distance-field shadows use a hard threshold when
their smoothing edges coincide, including after float rounding.

Regression coverage compiles named Make targets and checks dependency changes.
A real Metal readback reproduced the old zero-width threshold failure and now
passes for zero, tiny, and positive smoothing widths. The desktop build and all
422 CTest tests passed. These shader fixes are not a confirmed explanation of
the reported phone screenshot.
