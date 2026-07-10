# Gyroscope Binding Capture Sensitivity Design

## Goal

Make gyroscope controls easy to bind with a light deliberate motion while preventing resting sensor noise from becoming a binding.

## Root cause

`InputCaptureController` currently applies one hard-coded normalized activation threshold of `0.50` to every capture candidate. Gyroscope input arrives through the existing SDL joystick/controller axis path, so a user must produce at least half-scale motion before either axis direction can bind.

## Scope and constraints

- Change binding capture only. Gameplay activation thresholds, dead zones, saved profile defaults, and runtime input response remain unchanged.
- Treat gyroscope input as the `ControlKind::Axis` events it already produces; do not add a new device or control type.
- Preserve positive and negative axis directions as distinct physical controls.
- Keep digital buttons, hats, keys, MIDI notes, and MIDI controls at their existing capture behavior.
- Continue exposing all finite samples in the live input monitor, including noise that is too small to bind.
- Preserve held-input and disconnect safety: an already-active control must not bind again until it has re-armed, and disconnect must clear its capture state.

## Approaches considered

### Lower one global threshold

Lowering the existing constant is the smallest change, but it also changes MIDI control capture and allows an axis hovering near the threshold to repeatedly re-arm.

### Adaptive noise calibration

Measuring a per-device baseline and noise envelope could adapt to unusual sensors. It would add calibration delay and more state, while gyro motion already has a normalized zero-centered signal. This complexity is not justified for the reported problem.

### Axis-specific hysteresis

Use a lower activation threshold for axes and a separate lower release threshold. This makes deliberate motion easy to detect, filters resting jitter, retains immediate capture, and does not change unrelated control types. This is the selected approach.

## Capture behavior

Each signed axis direction keeps its existing independent active state.

- An inactive axis direction becomes active at a normalized magnitude of `0.20` or greater. Its inactive-to-active edge is eligible to create a binding.
- An active axis direction remains active while its magnitude is above `0.10`.
- At `0.10` or below, that direction becomes inactive and is re-armed for a later capture.
- Values between `0.10` and `0.20` preserve the prior state. They cannot activate an idle direction or re-arm an active one.
- Non-axis controls retain the existing `0.50` edge threshold. Their normal digital values of zero and one behave exactly as before.

The controller continues observing events while the settings UI is not listening. Therefore a gyro direction already held above the activation threshold when capture begins will not bind accidentally; the user must first return it to the release region and then move deliberately.

## Data flow and ownership

SDL axis normalization and `InputDeviceRegistry` remain unchanged. `InputCaptureController` selects capture thresholds from the candidate's `ControlKind`, updates the per-direction activation state with hysteresis, and stages a binding only on an inactive-to-active transition while listening. Persistence, conflict confirmation, profile replacement, and device-disconnect handling remain unchanged.

## Test strategy

The focused input-capture test will be changed first and observed failing against the current implementation. It will verify:

- positive and negative axis values below `0.20` remain unbound;
- an axis binds at exactly `0.20`;
- an active axis does not re-arm while its value remains between `0.10` and `0.20`;
- returning to `0.10` re-arms the axis and a later `0.20` crossing can bind;
- low-amplitude noise remains visible in the monitor without saving a binding;
- digital and MIDI capture behavior remains unchanged.

Verification will include the focused `foundation_input_capture` CTest, related input tests, the desktop `main` target, the full CTest suite, and a clean diff check.

## Acceptance criteria

- A light axis or gyro motion reaching 20% normalized magnitude can be bound.
- Resting motion at or below 10% cannot bind; it only re-arms a held direction.
- Mid-band jitter does not create another capture edge.
- No gameplay sensitivity or persisted binding defaults change.
- Existing non-axis capture behavior remains compatible.
