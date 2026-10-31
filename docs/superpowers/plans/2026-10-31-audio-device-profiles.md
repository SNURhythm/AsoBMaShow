# Audio Device Profiles Implementation Plan

**Goal:** Independently remember timing and keysound mode for Bluetooth and other outputs.
**Architecture:** Retain existing active settings fields; snapshot them on selection
and serialization. Native backend route metadata selects the active profile outside
songs and calibration; unknown routes do not change it.
**Tech Stack:** C++, platform audio APIs, JSON settings store.
**Spec:** docs/superpowers/specs/2026-10-31-audio-device-profiles-design.md

## Constraints and review focus

Migrate both profiles from legacy values. Keep profiles independent across restarts
and player switches. Preserve offsets during gameplay/calibration. Never classify
merely connected devices as the active route. Preserve unrelated local changes.

## Tasks

- [x] Extend app settings store tests for legacy migration and independent round trips.
- [x] Add audio timing profile state, selection and JSON persistence with validation.
- [x] Expose optional Bluetooth classification from the active audio backend on all platforms.
- [x] Select profiles in menus and settings; defer during gameplay and calibration.
- [x] Build main and affected test targets, run focused tests, inspect the diff, commit and push.

## Verification

- Desktop main and affected test groups build successfully.
- 70 tests in `area_audio`, `area_profiles`, and `area_settings` pass.
- Android debug Java compilation and native syntax checks pass.
- iOS route-query code passes an iPhoneOS SDK syntax check.
- Linux route code passes a native-header syntax check against PipeWire 0.3.48
  (using the available NDK compiler); graph traversal has native behavioral tests.
- Windows compilation and physical Bluetooth switching require target hardware.
- Linux uses native PipeWire graph metadata, including JACK clients under PipeWire;
  traditional JACK/custom routes without transport metadata stay unknown.
