# Music Player Playback Modes

## Goal

Add a music-player playback mode selector while retaining the existing 50–200% rate control.

- **Pitch Shift:** playback speed and pitch change together.
- **Time Stretch:** playback speed changes while musical pitch remains unchanged.

The selection is independent of gameplay playback settings and persists with the other music-player preferences. Existing settings default to Pitch Shift, preserving current behavior.

## UI and State

The transport's playback row contains a mode dropdown and the existing percentage dropdown. Changing either applies immediately to the loaded track, saves only after the native backend accepts the change, and otherwise keeps the prior selection while showing the backend error.

`AppSettings` stores `musicPlayerPlaybackMode` beside `musicPlayerPlaybackRatePercent`. `MusicPlayerService` owns the effective `audio::PlaybackRate` and reapplies both fields whenever a new track is loaded.

## Native Playback

The native bridge accepts one `audio::PlaybackRate` instead of only a percentage.

- Android maps Pitch Shift to `PlaybackParams.speed = rate` and `pitch = rate`; Time Stretch maps to `speed = rate` and `pitch = 1.0`.
- iOS uses `AVPlayerItem.audioTimePitchAlgorithm`: Varispeed links pitch to rate, while Spectral preserves pitch with Apple's music-oriented time-stretch algorithm. Existing load, play, pause, stop, seek, elapsed-time, metadata, queue, and remote-control behavior remains intact.

Desktop remains unchanged because the native music player is mobile-only.

## Validation

Rate validation remains 50–200% in 5% steps. Mode values are restricted to Pitch Shift and Time Stretch. Settings tests cover persistence, legacy defaults, and sanitization; service/native mapping is exercised through focused tests where platform-independent. Desktop compilation is required, with Android and iOS compile checks for the changed native code.
