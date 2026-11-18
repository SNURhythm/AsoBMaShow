# Input and controllers

## Intent and user flow

Players configure keyboard, controller, touch, MIDI, and motion controls per
profile, capture bindings, and use the resulting logical controls in gameplay.
The same logical transitions must remain deterministic for live input, replay
recording, gameplay practice, and visual-controller presentation.

## Code map

- `src/input/InputTypes.*`, `InputProfile.*`, `InputBindingResolver.*`, and
  `LogicalGameplayInputAdapter.*` implement the portable input model.
- SDL, MIDI, Android, iOS, Windows, gyroscope, touch, and virtual-controller
  files adapt device-specific APIs to normalized physical events.
- `InputCaptureController.*` owns capture, thresholds, conflicts, and binding
  replacement; settings scenes only present its state.
- Timestamp and callback-lifetime helpers protect cross-thread/native delivery.
- `src/scene/play/RealtimeGameplayInputRegistration.*` owns gameplay's native
  registry subscriptions, optional SDL watch, and device-class routing claims.

## Boundaries and invariants

Native callbacks enqueue normalized events and do not mutate gameplay directly.
The resolver is the single physical-to-logical mapping authority; device
backends must not invent alternate gameplay semantics. Persisted bindings use
versioned profile data, and capture rules use canonical defaults so missing
fields do not silently alter existing profiles.

Gameplay registers native callbacks while delivery is gated, enables scene
ingress, then activates backend claims. Shutdown closes ingress and detaches
native registrations before draining touches and joining the simulation worker.
Registration teardown waits for active callbacks and restores ordinary routing.

## Virtual controller customization

On supported touch platforms, Input settings save a separate controller layout
for each key mode. Start and Select have independent On/Off buttons; disabled
controls are neither drawn nor touchable. Scratch supports Flick, Spin, and
Touch. Touch presses immediately and holds until release, using the same
scratch lane/replay identity as the other methods (including command-only
scratch in scratchless modes).

**Edit layout** opens a full-screen canvas with three editing modes:

- **Whole layout:** drag the controller or the colored size/spacing handles.
- **Individual controls:** drag any key, scratch, Start, or Select to select and
  move it. The amber corner handle resizes it proportionally. Arrow buttons
  select overlapping controls. Reset restores only the selected control.
- **Touch area:** drag horizontally to increase/decrease compensation. Coral
  outlines show the expanded touch targets. Reset restores the default 50%.

Gestures update the preview locally and save on release. A cancelled gesture
rolls back without saving. The Input page also offers a control selector and
numeric size/offset fields with step and reset buttons. Size is 25–300% of the
control's base size; X/Y offsets are percentages of the base button width,
positive right/down. Individual edits keep neighboring controls fixed and keep
controls on the canvas. Whole-layout scaling applies to custom offsets too.

Use the −5° and +5° buttons in **Whole layout** to turn the plate, or in
**Individual controls** to turn the selected control about its center.
The numeric rotation fields accept exact degrees. Positive angles turn clockwise,
and a control’s individual angle is relative to the plate. Existing layouts
start at zero degrees.

Touch compensation is shared by all controls in that key mode. Its 0–200%
value adds that fraction of each control's shorter dimension to the hit area's
width and height (half on each edge). At 0% the touch target matches the visible
shape. At 50% it preserves the old fixed padding. Visual sizes are unaffected.
Input profile schema 12 persists these options; older profiles retain their
existing layout, enabled system buttons, and compensation defaults.

## Verification

Use `input_binding_resolver_tests`, `input_capture_controller_tests`,
`input_profile_tests`, `input_device_registry_tests`, `logical_gameplay_input_tests`,
`midi_input_tests`, gyroscope tests, and platform-specific input targets.
`realtime_gameplay_input_registration_tests` exercises the actual registry and
SDL watches; the gameplay terminal fixture checks the registration-to-worker
shutdown ordering and final replay transfer.

## Related pages

- [Gameplay and scoring](gameplay-and-scoring.md)
- [Profiles and data transfer](profiles-and-data-transfer.md)
- [Settings and user interface](settings-and-user-interface.md)
