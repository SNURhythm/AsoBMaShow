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

## Boundaries and invariants

Native callbacks enqueue normalized events and do not mutate gameplay directly.
The resolver is the single physical-to-logical mapping authority; device
backends must not invent alternate gameplay semantics. Persisted bindings use
versioned profile data, and capture rules use canonical defaults so missing
fields do not silently alter existing profiles.

## Verification

Use `input_binding_resolver_tests`, `input_capture_controller_tests`,
`input_profile_tests`, `input_device_registry_tests`, `logical_gameplay_input_tests`,
`midi_input_tests`, gyroscope tests, and platform-specific input targets.

## Related pages

- [Gameplay and scoring](gameplay-and-scoring.md)
- [Profiles and data transfer](profiles-and-data-transfer.md)
- [Settings and user interface](settings-and-user-interface.md)
