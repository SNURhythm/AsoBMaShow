# Gyroscope Binding Capture Sensitivity Design

## Goal

Make gyroscope controls easy to bind and use with a light deliberate motion while preventing resting sensor noise from becoming a binding. Newly created and repaired bindings will use matching `Activate 0.20` and `Release 0.10` defaults.

## Root cause

`InputCaptureController` currently applies one hard-coded normalized activation threshold of `0.50` to every capture candidate. Gyroscope input arrives through the existing SDL joystick/controller axis path, so a user must produce at least half-scale motion before either axis direction can bind. A captured binding also inherits the canonical `Activate 0.50` and `Release 0.35` values, so lowering capture sensitivity alone would still leave the resulting gyro binding too demanding during gameplay.

## Scope and constraints

- Change axis binding capture and the canonical activation/release defaults together.
- Treat gyroscope input as the `ControlKind::Axis` events it already produces; do not add a new device or control type.
- Preserve positive and negative axis directions as distinct physical controls.
- Keep digital buttons, hats, keys, MIDI notes, and MIDI controls at their existing capture behavior.
- Preserve every finite, explicitly saved activation/release pair in an existing profile. Do not rewrite valid user-customized values during load or migration.
- Continue exposing all finite samples in the live input monitor, including noise that is too small to bind.
- Preserve held-input and disconnect safety: an already-active control must not bind again until it has re-armed, and disconnect must clear its capture state.

## Approaches considered

### Lower one global threshold

Lowering the existing constant is the smallest change, but it also changes MIDI control capture and allows an axis hovering near the threshold to repeatedly re-arm.

### Adaptive noise calibration

Measuring a per-device baseline and noise envelope could adapt to unusual sensors. It would add calibration delay and more state, while gyro motion already has a normalized zero-centered signal. This complexity is not justified for the reported problem.

### Axis-specific hysteresis

Use a lower activation threshold for axes and a separate lower release threshold. This makes deliberate motion easy to detect, filters resting jitter, retains immediate capture, and does not change unrelated control types. This is the selected approach.

For binding defaults, changing only captured axis candidates would leave struct defaults, missing-field deserialization, and sanitizer recovery inconsistent. The selected design changes all canonical default and recovery sites to the same `0.20`/`0.10` pair while preserving valid explicit values already stored by users.

## Capture behavior

Each signed axis direction keeps its existing independent active state.

- An inactive axis direction becomes active at a normalized magnitude of `0.20` or greater. Its inactive-to-active edge is eligible to create a binding.
- An active axis direction remains active while its magnitude is above `0.10`.
- At `0.10` or below, that direction becomes inactive and is re-armed for a later capture.
- Values between `0.10` and `0.20` preserve the prior state. They cannot activate an idle direction or re-arm an active one.
- Non-axis controls retain the existing `0.50` edge threshold. Their normal digital values of zero and one behave exactly as before.

The controller continues observing events while the settings UI is not listening. Therefore a gyro direction already held above the activation threshold when capture begins will not bind accidentally; the user must first return it to the release region and then move deliberately.

## Binding defaults

- `InputBinding` defaults become `activationThreshold = 0.20` and `releaseThreshold = 0.10`.
- Newly captured bindings inherit those values, so the settings fields display `Activate 0.20` and `Release 0.10` immediately after capture.
- Profile deserialization uses the same pair when either field is absent.
- Sanitization uses the same pair when thresholds are non-finite or violate dead-zone/release/activation ordering.
- Valid thresholds explicitly present in an existing profile remain byte-for-value unchanged.

Digital bindings remain behaviorally unchanged because their normalized values are zero or one. Existing analog and MIDI-control bindings also remain unchanged when they already contain valid explicit thresholds; only newly created bindings and fallback recovery use the new defaults.

## Data flow and ownership

SDL axis normalization and `InputDeviceRegistry` remain unchanged. `InputCaptureController` selects capture thresholds from the candidate's `ControlKind`, updates the per-direction activation state with hysteresis, and stages a binding only on an inactive-to-active transition while listening. The input model and profile loader share the new canonical defaults; persistence format, conflict confirmation, profile replacement, and device-disconnect handling remain unchanged.

## Test strategy

The focused input-capture test will be changed first and observed failing against the current implementation. It will verify:

- positive and negative axis values below `0.20` remain unbound;
- an axis binds at exactly `0.20`;
- an active axis does not re-arm while its value remains between `0.10` and `0.20`;
- returning to `0.10` re-arms the axis and a later `0.20` crossing can bind;
- low-amplitude noise remains visible in the monitor without saving a binding;
- digital and MIDI capture behavior remains unchanged;
- a newly captured binding stores `Activate 0.20` and `Release 0.10`;
- missing, non-finite, and invalid threshold pairs recover to `0.20`/`0.10`;
- valid explicit legacy and customized thresholds remain unchanged.

Verification will include the focused `foundation_input_capture` CTest, related input tests, the desktop `main` target, the full CTest suite, and a clean diff check.

## Acceptance criteria

- A light axis or gyro motion reaching 20% normalized magnitude can be bound.
- Resting motion at or below 10% cannot bind; it only re-arms a held direction.
- Mid-band jitter does not create another capture edge.
- New and fallback bindings use `Activate 0.20` and `Release 0.10` in the settings UI and gameplay resolver.
- Valid activation/release values already saved by a user are not migrated or overwritten.
- Existing non-axis capture behavior remains compatible.
