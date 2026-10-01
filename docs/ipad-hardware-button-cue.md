# iPad Guided Access button cue

The offline hardware table maps `uname` machine identifiers to Home/Top buttons.
Simulator builds use `SIMULATOR_MODEL_IDENTIFIER`. Unknown hardware keeps generic
instructions and does not guess a button location. Four older bezel-free Pro
models have known Top buttons but no measured location in this snapshot, so they
also retain text-only instructions.

Sources:

- [DeviceKit model identifiers](https://github.com/devicekit/DeviceKit/blob/9000b09deb528298f493a69cae55663c8d85983e/Source/Device.swift.gyb).
- [AppMana dimensional table](https://github.com/AppMana/devices/blob/223924d6b9f33c83e50b2fb2f48b2b5e975ae55e/src/device_dimensions/devices.json), retrieved 2026-10-02.
- [Apple dimensional drawings](https://developer.apple.com/accessories/dimensional-drawings/).
- [Apple Guided Access instructions](https://support.apple.com/111795).
- [Data licenses](../assets/legal/ipad-device-data.txt).

`portraitX` is the button center projected onto the active display, with the
screen viewed upright from the front. For Top buttons, take the body-edge offset
plus half the button length, then subtract the side bezel and divide by the
active display width. Right-edge offsets are reflected first. The mini 6/A17 Pro
button is at the **left** of its top edge, opposite the volume buttons. Home
buttons project to the bottom center. We store six decimal places, not a claim
of subpixel physical accuracy. The marker is a visual guide, not calibration.

The Mini 6, Air 11 M4 (drawing shared with M2/M3), Pro 11 M4, and Pro 11 generation
3 drawing front/top projections were visually checked against the table.

| Model | Portrait display X | Measurement source |
| --- | ---: | --- |
| iPad (10th generation) | 0.915534 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-a16.pdf) |
| iPad (A16) | 0.915534 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-a16.pdf) |
| iPad Air 11-inch (M2) | 0.929248 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-air-11-inch-m4.pdf) |
| iPad Air 11-inch (M3) | 0.929248 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-air-11-inch-m4.pdf) |
| iPad Air 11-inch (M4) | 0.929248 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-air-11-inch-m4.pdf) |
| iPad Air 13-inch (M2) | 0.936466 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-air-13-inch-m4.pdf) |
| iPad Air 13-inch (M3) | 0.936466 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-air-13-inch-m4.pdf) |
| iPad Air 13-inch (M4) | 0.936466 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-air-13-inch-m4.pdf) |
| iPad Air (4th generation) | 0.929090 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-air-4th-generation.pdf) |
| iPad Air (5th generation) | 0.929090 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-air-5th-generation.pdf) |
| iPad mini (6th generation) | 0.105971 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-mini-6th-generation.pdf) |
| iPad mini (A17 Pro) | 0.105971 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-mini-a17-pro.pdf) |
| iPad Pro 11-inch (3rd generation) | 0.937597 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-pro-11-inch-3rd-generation.pdf) |
| iPad Pro 11-inch (4th generation) | 0.937597 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-pro-11-inch-4th-generation.pdf) |
| iPad Pro 11-inch (M4) | 0.929307 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-pro-11-inch-m4.pdf) |
| iPad Pro 11-inch (M5) | 0.929307 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-pro-11-inch-m5.pdf) |
| iPad Pro 12.9-inch (5th generation) | 0.951656 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-pro-12.9-inch-5th-generation.pdf) |
| iPad Pro 12.9-inch (6th generation) | 0.951656 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-pro-12.9-inch-6th-generation.pdf) |
| iPad Pro 13-inch (M4) | 0.940670 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-pro-13-inch-m4.pdf) |
| iPad Pro 13-inch (M5) | 0.940670 | [Drawing](https://developer.apple.com/download/files/accessories/dimensional-drawings/ipad-pro-13-inch-m5.pdf) |

Use the active window scene's **interface** orientation, not accelerometer/device
orientation. UIKit's landscape names differ from UIDeviceOrientation: interface
LandscapeLeft places Home on the left; LandscapeRight places Home on the right.
The normalized mappings from portrait `(x, y)` are `(1-y, x)` for LandscapeLeft,
`(y, 1-x)` for LandscapeRight, and `(1-x, 1-y)` upside down.

A location is shown only when the app fills the built-in screen. Unknown
orientation, external displays, and partial windows use the existing centered
instructions without a duplicate floating label. Back and Skip this session are
large text buttons in the central controls, leaving all screen edges free.
The cue is recalculated while the reminder is visible, and confirms alongside
the central lock when Guided Access activates: a green
marker stretch and rebound, a separate Font Awesome check beside localized
Enabled text. The spring settles in place and the success cue remains fully
visible until gameplay starts. This adds no time to the existing two-second
startup delay. Interruption resets the cue to idle.
Text stays upright in app coordinates; the edge marker changes axis with orientation.

Skipping is kept only for the current gameplay session, including in-game and
result-screen retries, practice restarts, and course continuation. Returning to
song selection and starting again creates a fresh reminder session. The choice
is not saved in profile settings or replay data.
