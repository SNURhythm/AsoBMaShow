# Audio device timing profiles

Keep audio offset, visual offset, and Input Trigger / Auto Timed keysound mode in
separate Bluetooth and non-Bluetooth profiles. Migrate existing values to both
profiles, including legacy config imports. Thereafter edits affect only the active
profile. Detect the actual output route on iOS, Android, macOS, Windows and Linux;
unknown/transitional routes retain the last selection. Defer switching during
songs and calibration to avoid changing timing underneath an active session.

## Approach

The user selected automatic route selection over a manual profile selector.
Keep the existing settings controls and playback fields as the active values;
store both profiles alongside them. Poll route metadata outside gameplay and
calibration. Do not infer transport from a user-editable device name.
